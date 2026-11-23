/**
 * @file model_page.cpp
 * @version 1.3.2
 * @brief Implementierung der Kanalliste und Modell-Speicherverwaltung mit umfassenden
 *        Zustands- und Trace-Meldungen ([MODEL_PAGE][TRACE]) zur Fehler- und Crash-Eingrenzung,
 *        automatischer Anzeige/Aktualisierung des verbundenen Modells, Modellwechsel-Abfrage
 *        ("Speichern oder verwerfen"), "Configs *"-Tabmarkierung, "Config loeschen"-Button
 *        und "Modelle *"-Dirty-Tracking.
 * @Version_info v1.3.2: Umfangreiche serielle Zustands-, Pointer-, Index- und Lifecycle-Logs
 *               Diagnoseausgaben auf das dynamische SerialInterface umgestellt;
 *               LV_EVENT_DELETE-Callbacks zur automatischen Nullung freigegebener LVGL-Zeiger
 *               (channel_list_subpage, ta_model_name, btn_send_ref) eingebaut.
 *               v1.3.1: Debug-Ausgabe in check_config_changes() nur noch bei tatsaechlicher
 *               Zustandsaenderung; sync_current_config_with_connected_model() ueberschreibt
 *               laufende Aenderungen an currentConfig nicht mehr; Bounds-Checks ergaenzt.
 */


#include <lvgl.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include "config_types.h"
#include "hardware_io.h"
#include "espnow_comm.h"
#include "model_page.h"
#include "models_page.h"
#include "channel_subpage.h"
#include "audio_module.h"
#include "SerialInterface.h"
#include "pairing_manager.h"
#include "core_tasks.h"

#pragma region MODEL_PAGE_GLOBALS
// Globale UI-Referenzen und Statusvariablen fuer das Modell
lv_obj_t *channel_list_subpage = NULL;
lv_obj_t *ta_model_name = NULL;
static lv_obj_t *file_list_popup = NULL;
static lv_obj_t *mbox_delete_confirm = NULL;
static lv_obj_t *mbox_save_overwrite = NULL;
static lv_obj_t *mbox_overwrite = NULL;
static lv_obj_t *mbox_load_warning = NULL;
static lv_obj_t *mbox_switch_unsaved = NULL;
static lv_obj_t *btn_send_ref = NULL;
static lv_obj_t *btn_save_ref = NULL;
static char pending_load_model_name[MAX_NAME_LEN] = "";
static int pending_switch_model_index = -1;
static bool is_holding_send_btn = false;
static unsigned long send_btn_press_start_ms = 0;
static bool is_success_feedback_active = false;
static unsigned long success_feedback_start_ms = 0;

// Dirty-Status je gepairtem Modell-Slot fuer pairedModels[idx].config
static bool paired_config_dirty[MAX_PAIRED_MODELS] = { false };
static int8_t last_tracked_model_idx = -127;
static bool last_tracked_connected = false;

extern PairedModelInfo *pairedModels;
extern int8_t activeModelIndex;

// Optionale externe Hooks aus anderen Modulen (falls vorhanden, schwach/extern angebunden)
extern void apply_model_switch_from_ui(int newModelIndex) __attribute__((weak));
extern bool has_channel_subpage_unsaved_changes(void) __attribute__((weak));
extern void commit_channel_subpage_changes(void) __attribute__((weak));
extern void discard_channel_subpage_changes(void) __attribute__((weak));

static void update_config_action_button(lv_obj_t *button) {
  if (!button || !lv_obj_is_valid(button)) return;

  if (is_config_changed) {
    lv_obj_clear_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(button, lv_palette_main(LV_PALETTE_GREEN), 0);
  } else {
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(button, lv_palette_main(LV_PALETTE_GREY), 0);
  }
}

static void update_config_action_buttons(void) {
  update_config_action_button(btn_save_ref);
  update_config_action_button(btn_send_ref);
}

/**
 * @brief Gibt eine detaillierte Zustands- und Speicherdiagnose der model_page aus.
 * @param step Bezeichner des aktuellen Ausfuehrungsschritts.
 */
static void log_model_page_state(const char *step) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] [%s] | activeIdx=%d | pairedModels=%p | hasTel=%d | ageTel=%lums | tab_cfg=%p(valid=%d) | list_sub=%p(valid=%d) | ch_sub=%p(valid=%d) | chOpen=%d | cfgChanged=%d | chCnt=%d | model='%s'\n",
                step,
                (int)activeModelIndex,
                (void *)pairedModels,
                (int)hasTelemetry,
                (unsigned long)(millis() - lastTelemetryTime),
                (void *)tab_configs,
                (tab_configs != NULL) ? (int)lv_obj_is_valid(tab_configs) : 0,
                (void *)channel_list_subpage,
                (channel_list_subpage != NULL) ? (int)lv_obj_is_valid(channel_list_subpage) : 0,
                (void *)channel_config_subpage,
                (channel_config_subpage != NULL) ? (int)lv_obj_is_valid(channel_config_subpage) : 0,
                (int)is_config_page_open,
                (int)is_config_changed,
                (int)currentConfig.channelCount,
                currentConfig.modelName);
  
  }
}

/**
 * @brief Gibt den aktuellen VRAM-Speicherstatus von LVGL im seriellen Monitor aus.
 * @param tag Bezeichner fuer die Diagnose-Ausgabe.
 */
static void print_vram_status_model(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Groesster freier Block: %u Bytes\n",
                tag, (int)mon.used_pct, (unsigned int)mon.free_biggest_size);
  
  }
}

/**
 * @brief LVGL Delete-Hook fuer channel_list_subpage, verhindert Dangling Pointer bei externem Clean.
 */
static void channel_list_subpage_deleted_cb(lv_event_t *e) {
  lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] LV_EVENT_DELETE fuer channel_list_subpage=%p (aktuell=%p)\n",
                (void *)target, (void *)channel_list_subpage);
  
  }
  if (target == channel_list_subpage) {
    channel_list_subpage = NULL;
    ta_model_name = NULL;
    btn_send_ref = NULL;
    btn_save_ref = NULL;
  }
}

/**
 * @brief Prueft, ob aktuell ein gueltiger gepairter Modell-Slot aktiv ist.
 * @return true, wenn pairedModels != NULL, activeModelIndex gueltig und isPaired == true ist.
 */
static bool has_valid_active_paired_slot(void) {
  return (pairedModels != NULL &&
          activeModelIndex >= 0 &&
          activeModelIndex < MAX_PAIRED_MODELS &&
          pairedModels[activeModelIndex].isPaired);
}

/**
 * @brief Prueft, ob aktuell ein Modell aktiv verbunden ist (Telemetrie-Timeout < 2000 ms).
 * @return true, wenn eine aktive Verbindung zu einem gepairten Modell besteht.
 */
bool is_any_paired_model_connected(void) {
  if (!has_valid_active_paired_slot()) return false;
  bool tele_fresh = (hasTelemetry && ((millis() - lastTelemetryTime) < 2000));
  bool rcv_flag = (pairedModels[activeModelIndex].telemetry.configReceived == 1);
  return (tele_fresh || rcv_flag);
}

/**
 * @brief Liefert den Index des aktuell verbundenen Modells oder -1, wenn keines verbunden ist.
 * @return Modell-Index >= 0 oder -1.
 */
int get_connected_paired_model_index(void) {
  if (is_any_paired_model_connected() && has_valid_active_paired_slot()) {
    return (int)activeModelIndex;
  }
  return -1;
}

/**
 * @brief Synchronisiert currentConfig und lastSavedConfig mit dem verbundenen Modell,
 *        falls ein Modell verbunden ist und keine ungespeicherten Aenderungen anliegen.
 * @param forceOverwrite Wenn true, wird currentConfig auch bei laufenden Aenderungen ueberschrieben.
 * @return true, wenn eine aktive Verbindung vorlag und synchronisiert wurde.
 */
static bool sync_current_config_with_connected_model(bool forceOverwrite = false) {
  int conn_idx = get_connected_paired_model_index();
  if (conn_idx < 0) return false;

  if (!forceOverwrite && (is_config_changed || paired_config_dirty[conn_idx])) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] sync_current_config uebersprungen (force=0, is_config_changed=%d, dirty[%d]=%d)\n",
                  (int)is_config_changed, conn_idx, (int)paired_config_dirty[conn_idx]);
  }
    return true;
  }

  if (pairedModels[conn_idx].config.channelCount > 0 || strlen(pairedModels[conn_idx].config.modelName) > 0) {
    ModelConfig activeConfig = pairedModels[conn_idx].config;
    if (strlen(activeConfig.modelName) == 0 && strlen(pairedModels[conn_idx].modelName) > 0) {
      strncpy(activeConfig.modelName, pairedModels[conn_idx].modelName, MAX_NAME_LEN - 1);
      activeConfig.modelName[MAX_NAME_LEN - 1] = '\0';
    }
    portENTER_CRITICAL(&configMux);
    currentConfig = activeConfig;
    portEXIT_CRITICAL(&configMux);
    lastSavedConfig = activeConfig;
    is_config_changed = false;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] sync_current_config ausgefuehrt von pairedModels[%d] ('%s', ch=%d)\n",
                  conn_idx, currentConfig.modelName, (int)currentConfig.channelCount);
  }
  }
  return true;
}

/**
 * @brief Setzt den Dirty-Status fuer pairedModels[idx].config und aktualisiert "Modelle *".
 * @param idx Index des gepairten Modells.
 * @param dirty true = ungespeicherte Aenderungen vorhanden, false = gespeichert/sauber.
 */
void mark_paired_model_config_dirty(int idx, bool dirty) {
  if (idx < 0 || idx >= MAX_PAIRED_MODELS) return;
  paired_config_dirty[idx] = dirty;
  refresh_models_tab_star_indicator();
  refresh_models_list_ui();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] pairedModels[%d].config Dirty-Status -> %s\n", idx, dirty ? "JA (*)" : "NEIN");
  }
}

/**
 * @brief Liefert zurueck, ob pairedModels[idx].config ungespeicherte Aenderungen hat.
 * @param idx Index des gepairten Modells.
 * @return true, wenn dirty.
 */
bool is_paired_model_config_dirty(int idx) {
  if (idx < 0 || idx >= MAX_PAIRED_MODELS) return false;
  return paired_config_dirty[idx];
}

/**
 * @brief Prueft, ob mindestens ein gepairtes Modell ungespeicherte Config-Aenderungen hat.
 * @return true, wenn ein Slot dirty ist.
 */
bool has_any_paired_model_config_dirty(void) {
  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (paired_config_dirty[i]) return true;
  }
  return false;
}

/**
 * @brief Speichert pairedModels[idx].config persistent im Flash und setzt den Dirty-Status zurueck.
 * @param idx Index des gepairten Modells.
 */
void save_paired_model_config_by_index(int idx) {
  log_model_page_state("save_paired_model_config_by_index:ENTER");
  if (!pairedModels || idx < 0 || idx >= MAX_PAIRED_MODELS || !pairedModels[idx].isPaired) return;
  if (strlen(pairedModels[idx].config.modelName) == 0 && strlen(pairedModels[idx].modelName) > 0) {
    strncpy(pairedModels[idx].config.modelName, pairedModels[idx].modelName, MAX_NAME_LEN - 1);
    pairedModels[idx].config.modelName[MAX_NAME_LEN - 1] = '\0';
  }
  saveConfigToFlash(pairedModels[idx].config);
  save_paired_models_to_fs();
  paired_config_dirty[idx] = false;
  if (idx == (int)activeModelIndex) {
    lastSavedConfig = currentConfig;
    is_config_changed = false;
    check_config_changes();
  }
  refresh_models_tab_star_indicator();
  refresh_models_list_ui();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] pairedModels[%d].config persistent gespeichert.\n", idx);
  }
}

/**
 * @brief Aktualisiert die Stern-Markierung ("Modelle" / "Modelle *") am Tab "Modelle".
 */
void refresh_models_tab_star_indicator(void) {
  static int last_models_star_state = -1;
  bool any_dirty = has_any_paired_model_config_dirty();
  int state_now = any_dirty ? 1 : 0;

  if (!tab_configs || !lv_obj_is_valid(tab_configs)) return;

  lv_obj_t *tab_content_area = lv_obj_get_parent(tab_configs);
  if (!tab_content_area) return;
  lv_obj_t *tabview = lv_obj_get_parent(tab_content_area);
  if (!tabview) return;
  lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
  if (!tab_bar) return;

  uint32_t child_cnt = lv_obj_get_child_count(tab_bar);
  for (uint32_t i = 0; i < child_cnt; i++) {
    lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
    if (!btn) continue;
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (!lbl) continue;
    const char *txt = lv_label_get_text(lbl);
    if (txt && (strncmp(txt, "Modelle", 7) == 0)) {
      const char *target_txt = any_dirty ? "Modelle *" : "Modelle";
      if (strcmp(txt, target_txt) != 0) {
        lv_label_set_text(lbl, target_txt);
        lv_obj_invalidate(lbl);
      }
      if (state_now != last_models_star_state) {
        last_models_star_state = state_now;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
        SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Tab 'Modelle' Markierung aktualisiert -> %s\n", target_txt);
  }
      }
      break;
    }
  }
}

/**
 * @brief Uebernimmt eine empfangene ModelConfig in das aktuell aktive gepairte Modell
 *        und speichert die gepairten Modelle persistent in LittleFS.
 * @param incomingCfg Empfangene Modellkonfiguration.
 */
static void store_config_into_active_paired_model(const ModelConfig &incomingCfg) {
  if (has_valid_active_paired_slot()) {
    if (memcmp(&pairedModels[activeModelIndex].config, &incomingCfg, sizeof(ModelConfig)) == 0) {
      refresh_models_list_ui();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
      SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Config ('%s', %d Kanaele) in pairedModels[%d].config bereits identisch vorhanden.\n",
                    pairedModels[activeModelIndex].config.modelName,
                    (int)pairedModels[activeModelIndex].config.channelCount,
                    (int)activeModelIndex);
  }
      return;
    }

    pairedModels[activeModelIndex].config = incomingCfg;
    if (strlen(pairedModels[activeModelIndex].config.modelName) == 0 &&
        strlen(pairedModels[activeModelIndex].modelName) > 0) {
      strncpy(pairedModels[activeModelIndex].config.modelName,
              pairedModels[activeModelIndex].modelName,
              MAX_NAME_LEN - 1);
      pairedModels[activeModelIndex].config.modelName[MAX_NAME_LEN - 1] = '\0';
    }
    paired_config_dirty[activeModelIndex] = false;
    save_paired_models_to_fs();
    refresh_models_tab_star_indicator();
    refresh_models_list_ui();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Config ('%s', %d Kanaele) in pairedModels[%d].config uebernommen und persistent gespeichert.\n",
                  pairedModels[activeModelIndex].config.modelName,
                  (int)pairedModels[activeModelIndex].config.channelCount,
                  (int)activeModelIndex);
  }
  }
}

/**
 * @brief Fuehrt den eigentlichen Modellwechsel nach Freigabe oder Verwerfen aus.
 * @param newModelIndex Neuer aktiver Modell-Index.
 */
static void execute_model_switch_now(int newModelIndex) {
  log_model_page_state("execute_model_switch_now:ENTER");
  if (is_config_page_open && discard_channel_subpage_changes) {
    discard_channel_subpage_changes();
  }
  is_config_page_open = false;
  is_config_changed = false;
  check_config_changes();

  if (apply_model_switch_from_ui) {
    apply_model_switch_from_ui(newModelIndex);
  } else {
    activeModelIndex = (int8_t)newModelIndex;
  }

  last_tracked_model_idx = activeModelIndex;
  sync_current_config_with_connected_model(true);
  lastSavedConfig = currentConfig;

  if (channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
    build_channel_list_page();
  }
  refresh_models_list_ui();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Modellwechsel auf Index %d ausgefuehrt und Kanalliste aktualisiert.\n", newModelIndex);
  }
}

/**
 * @brief Callback fuer die Abfrage "Speichern oder verwerfen" vor dem Modellwechsel.
 */
static void mbox_switch_unsaved_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  if (code != LV_EVENT_CLICKED) return;

  const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));
  int target_idx = pending_switch_model_index;
  pending_switch_model_index = -1;

  if (mbox_switch_unsaved && lv_obj_is_valid(mbox_switch_unsaved)) {
    lv_msgbox_close(mbox_switch_unsaved);
  }
  mbox_switch_unsaved = NULL;

  if (btn_txt && strcmp(btn_txt, "Speichern") == 0) {
    if (is_config_page_open && commit_channel_subpage_changes) {
      commit_channel_subpage_changes();
    }
    int conn_idx = get_connected_paired_model_index();
    if (conn_idx >= 0) {
      pairedModels[conn_idx].config = currentConfig;
      mark_paired_model_config_dirty(conn_idx, true);
    } else {
      saveConfigToFlash(currentConfig);
    }
    execute_model_switch_now(target_idx);
  } else if (btn_txt && strcmp(btn_txt, "Verwerfen") == 0) {
    currentConfig = lastSavedConfig;
    is_config_changed = false;
    check_config_changes();
    execute_model_switch_now(target_idx);
  }
}

/**
 * @brief Prueft vor einem Modellwechsel auf ungespeicherte Aenderungen in Kanalseite oder Config
 *        und fragt ggf. "Speichern oder verwerfen" ab.
 * @param newModelIndex Ziel-Modell-Index.
 */
void request_model_switch_with_guard(int newModelIndex) {
  log_model_page_state("request_model_switch_with_guard:ENTER");
  bool subpage_dirty = (is_config_page_open &&
                        (!has_channel_subpage_unsaved_changes || has_channel_subpage_unsaved_changes()));
  check_config_changes();
  bool has_unsaved = (subpage_dirty || is_config_changed);

  if (!has_unsaved) {
    execute_model_switch_now(newModelIndex);
    return;
  }

  pending_switch_model_index = newModelIndex;
  if (mbox_switch_unsaved && lv_obj_is_valid(mbox_switch_unsaved)) {
    lv_msgbox_close(mbox_switch_unsaved);
  }
  mbox_switch_unsaved = NULL;

  lv_obj_t *parent_obj = (tab_configs && lv_obj_is_valid(tab_configs)) ? tab_configs : lv_screen_active();
  mbox_switch_unsaved = lv_msgbox_create(parent_obj);
  if (!mbox_switch_unsaved) return;

  lv_msgbox_add_title(mbox_switch_unsaved, "Ungespeicherte Aenderungen");
  lv_msgbox_add_text(mbox_switch_unsaved,
                     subpage_dirty ? "In der Kanalseite liegen ungespeicherte Aenderungen vor.\nSpeichern oder verwerfen?"
                                   : "An der Config liegen ungespeicherte Aenderungen vor.\nSpeichern oder verwerfen?");

  lv_obj_t *b_save = lv_msgbox_add_footer_button(mbox_switch_unsaved, "Speichern");
  lv_obj_t *b_disc = lv_msgbox_add_footer_button(mbox_switch_unsaved, "Verwerfen");

  if (b_save && b_disc) {
    lv_obj_set_style_bg_color(b_save, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_color(b_disc, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(b_save, mbox_switch_unsaved_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_disc, mbox_switch_unsaved_cb, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_center(mbox_switch_unsaved);
}

/**
 * @brief Hook bei Modellwechsel: aktualisiert die Kanalliste des verbundenen Modells.
 * @param newModelIndex Neuer aktiver Modell-Index.
 */
void on_model_page_active_model_changed(int newModelIndex) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] on_model_page_active_model_changed(newModelIndex=%d)\n", newModelIndex);
  
  }
  last_tracked_model_idx = (int8_t)newModelIndex;
  if (pairedModels != NULL &&
      newModelIndex >= 0 &&
      newModelIndex < MAX_PAIRED_MODELS &&
      pairedModels[newModelIndex].isPaired) {
    ModelConfig activeConfig = pairedModels[newModelIndex].config;
    if (activeConfig.channelCount > 0 || activeConfig.modelName[0] != '\0') {
      if (activeConfig.modelName[0] == '\0') {
        strncpy(activeConfig.modelName, pairedModels[newModelIndex].modelName, MAX_NAME_LEN - 1);
        activeConfig.modelName[MAX_NAME_LEN - 1] = '\0';
      }
      portENTER_CRITICAL(&configMux);
      currentConfig = activeConfig;
      portEXIT_CRITICAL(&configMux);
      lastSavedConfig = activeConfig;
      is_config_changed = false;
    }
  }
  if (channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
    build_channel_list_page();
  }
}
#pragma endregion

#pragma region MODEL_PAGE_PERSISTENCE_AND_LIFECYCLE
/**
 * @brief Raeumt den Speicher der Kanallisten-Seite beim Verlassen vollstaendig ab.
 */
void cleanup_channel_list_page(void) {
  log_model_page_state("cleanup_channel_list_page:ENTER");
  if (channel_config_subpage && lv_obj_is_valid(channel_config_subpage)) {
    lv_obj_delete(channel_config_subpage);
  }
  channel_config_subpage = NULL;

  if (channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
    lv_obj_delete(channel_list_subpage);
  }
  channel_list_subpage = NULL;

  ta_model_name = NULL;
  btn_send_ref = NULL;
  btn_save_ref = NULL;
  is_config_page_open = false;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Kanallisten-Seite Speicher vollstaendig bereinigt.");
  }
  print_vram_status_model("Kanallisten-Seite Cleanup");
}

/**
 * @brief Speichert eine Modellkonfiguration im Flash-Speicher.
 */
void saveConfigToFlash(const ModelConfig &config) {
  char filename[32];
  snprintf(filename, sizeof(filename), "/cfg_%s.bin", config.modelName);
  fs::File file = LittleFS.open(filename, FILE_WRITE);
  if (!file) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Fehler: Konnte Konfigurationsdatei nicht zum Schreiben oeffnen!");
  }
    return;
  }
  file.write((const uint8_t *)&config, sizeof(ModelConfig));
  file.close();
  lastSavedConfig = currentConfig;  // Speicherstand synchronisieren
  saveLastUsedModelName(config.modelName);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Modell '%s' erfolgreich im Flash gespeichert.\n", config.modelName);
  }
}

/**
 * @brief Laedt eine Modellkonfiguration aus dem Flash-Speicher.
 */
bool loadConfigFromFlash(const char *modelName, ModelConfig &config) {
  char filename[32];
  snprintf(filename, sizeof(filename), "/cfg_%s.bin", modelName);
  fs::File file = LittleFS.open(filename, FILE_READ);
  if (!file) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Konfigurationsdatei '%s' nicht gefunden.\n", filename);
  }
    return false;
  }
  file.read((uint8_t *)&config, sizeof(ModelConfig));
  file.close();
  lastSavedConfig = currentConfig;  // Geladenen Stand als gesichert markieren
  saveLastUsedModelName(config.modelName);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Modell '%s' erfolgreich aus Flash geladen.\n", modelName);
  }
  return true;
}

/**
 * @brief Speichert den Namen des zuletzt genutzten Modells.
 */
void saveLastUsedModelName(const char *modelName) {
  fs::File file = LittleFS.open("/last_model.txt", FILE_WRITE);
  if (!file) return;
  file.print(modelName);
  file.close();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models,
                                "Zuletzt genutztes Modell gemerkt: %s", modelName);
  }
}

/**
 * @brief Laedt beim Systemstart das zuletzt genutzte Modell.
 */
void loadLastUsedModelOnBoot(void) {
  if (LittleFS.exists("/last_model.txt")) {
    fs::File file = LittleFS.open("/last_model.txt", FILE_READ);
    if (!file) return;

    char saved_name[MAX_NAME_LEN];
    int len = file.readBytes(saved_name, MAX_NAME_LEN - 1);
    saved_name[len] = '\0';
    file.close();

    SerialInterface::printDebug(SerialInterface::Module::Models,
                                "Boot-Autoload: Versuche Modell zu laden: %s", saved_name);

    if (loadConfigFromFlash(saved_name, currentConfig)) {
      build_channel_list_page();
    }
  } else {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "Kein voriges Modell fuer Autoload gefunden. Nutze Standardwerte.");
  }
  }
}
#pragma endregion

#pragma region MODEL_PAGE_CALLBACKS
// Forward-Deklarationen von Callbacks
static void mbox_save_overwrite_cb(lv_event_t *e);
static void mbox_overwrite_cb(lv_event_t *e);
static void mbox_delete_confirm_cb(lv_event_t *e);
static void mbox_load_warning_cb(lv_event_t *e);

/**
 * @brief Asynchroner Callback zum Oeffnen der Kanal-Konfiguration mit Bounds- und Pointer-Pruefung.
 * @param arg Zeiger auf den Kanal-Index (als intptr_t gecastet).
 */
static void open_channel_config_async_cb(void *arg) {
  intptr_t idx = (intptr_t)arg;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] open_channel_config_async_cb:ENTER idx=%d\n", (int)idx);
  }
  log_model_page_state("open_channel_config_async_cb:BEFORE_OPEN");

  if (idx < 0 || idx >= (intptr_t)currentConfig.channelCount || idx >= MAX_CHANNELS) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][ERROR] open_channel_config_async_cb abgebrochen: ungueltiger Kanal-Index %d (count=%d)\n",
                  (int)idx, (int)currentConfig.channelCount);
    
  }
    return;
  }
  if (!tab_configs || !lv_obj_is_valid(tab_configs)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][ERROR] open_channel_config_async_cb abgebrochen: tab_configs ungueltig!");
    
  }
    return;
  }

  open_channel_config_page((int)idx);
  log_model_page_state("open_channel_config_async_cb:AFTER_OPEN");
}

/**
 * @brief Callback beim Klick auf einen Kanal in der Liste.
 */
static void channel_click_cb(lv_event_t *e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] channel_click_cb: CLICK auf Kanal-Index=%d (channelCount=%d)\n",
                (int)idx, (int)currentConfig.channelCount);
  
  }
  if (idx < 0 || idx >= (intptr_t)currentConfig.channelCount || idx >= MAX_CHANNELS) return;
  lv_async_call(open_channel_config_async_cb, (void *)idx);
}

static void move_channel_order(int channel_idx, int direction) {
  int target_idx = channel_idx + direction;
  if (channel_idx < 0 || channel_idx >= currentConfig.channelCount ||
      target_idx < 0 || target_idx >= currentConfig.channelCount ||
      channel_idx >= MAX_CHANNELS || target_idx >= MAX_CHANNELS) {
    return;
  }

  ChannelConfig temp = currentConfig.channels[channel_idx];
  currentConfig.channels[channel_idx] = currentConfig.channels[target_idx];
  currentConfig.channels[target_idx] = temp;

  if (active_channel_idx == channel_idx) active_channel_idx = target_idx;
  else if (active_channel_idx == target_idx) active_channel_idx = channel_idx;

  int conn_idx = get_connected_paired_model_index();
  if (conn_idx >= 0) {
    pairedModels[conn_idx].config = currentConfig;
    mark_paired_model_config_dirty(conn_idx, true);
  }

  check_config_changes();
  build_channel_list_page();
  trigger_audio_feedback(AUDIO_EVENT_CLICK);
}

static void channel_move_up_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  move_channel_order(idx, -1);
}

static void channel_move_down_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  move_channel_order(idx, 1);
}

/**
 * @brief Asynchroner Callback zum Hinzufuegen und Oeffnen eines neuen Kanals.
 */
static void add_channel_async_cb(void *arg) {
  (void)arg;
  log_model_page_state("add_channel_async_cb:ENTER");

  if (currentConfig.channelCount < 0) currentConfig.channelCount = 0;
  if (currentConfig.channelCount >= MAX_CHANNELS) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] add_channel_async_cb abgebrochen: MAX_CHANNELS (%d) erreicht.\n", MAX_CHANNELS);
    
  }
    return;
  }

  int new_idx = (int)currentConfig.channelCount;
  currentConfig.channelCount++;
  memset(&currentConfig.channels[new_idx], 0, sizeof(ChannelConfig));
  snprintf(currentConfig.channels[new_idx].name, MAX_NAME_LEN, "CH %d", new_idx + 1);
  currentConfig.channels[new_idx].type = TYPE_ANALOG;
  currentConfig.channels[new_idx].behaviour = BEHAVIOUR_RESTORE_ZERO;

  int conn_idx = get_connected_paired_model_index();
  if (conn_idx >= 0) {
    pairedModels[conn_idx].config = currentConfig;
    mark_paired_model_config_dirty(conn_idx, true);
  }

  check_config_changes();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] add_channel_async_cb: rufe open_channel_config_page(%d) auf...\n", new_idx);
  
  }
  open_channel_config_page(new_idx);
  log_model_page_state("add_channel_async_cb:AFTER_OPEN");
}

/**
 * @brief Callback zum Hinzufuegen eines neuen Kanals (sicher via lv_async_call).
 */
static void add_channel_cb(lv_event_t *e) {
  (void)e;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] add_channel_cb: Button 'Weiteren Kanal hinzufuegen' geklickt (currentCount=%d, MAX=%d)\n",
                (int)currentConfig.channelCount, MAX_CHANNELS);
  
  }
  if (currentConfig.channelCount < MAX_CHANNELS) {
    lv_async_call(add_channel_async_cb, NULL);
  }
}

/**
 * @brief Fuehrt den eigentlichen lokalen Speichervorgang aus:
 *        Bei aktiver Modellverbindung direkt pairedModels[idx].config updaten,
 *        sonst wie bisher im Filesystem speichern.
 */
static void perform_local_save(void) {
  log_model_page_state("perform_local_save:ENTER");
  int conn_idx = get_connected_paired_model_index();
  if (conn_idx >= 0) {
    pairedModels[conn_idx].config = currentConfig;
    lastSavedConfig = currentConfig;
    is_config_changed = false;
    check_config_changes();
    mark_paired_model_config_dirty(conn_idx, true);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'lokal speichern' bei aktiver Verbindung: pairedModels[%d].config direkt aktualisiert.\n", conn_idx);
  }
    build_channel_list_page();
    return;
  }

  saveConfigToFlash(currentConfig);
  is_config_changed = false;
  check_config_changes();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'lokal speichern' ohne aktive Verbindung: im Filesystem (Flash) gespeichert.");
  }
  build_channel_list_page();
}

/**
 * @brief Callback zum lokalen Speichern der Konfiguration.
 */
static void btn_flash_save_cb(lv_event_t *e) {
  (void)e;
  log_model_page_state("btn_flash_save_cb:ENTER");
  if (strlen(currentConfig.modelName) == 0) {
    snprintf(currentConfig.modelName, MAX_NAME_LEN, "Unbekannt");
  }

  // Bei aktiver Verbindung direkt pairedModels[idx].config aktualisieren
  if (get_connected_paired_model_index() >= 0) {
    perform_local_save();
    return;
  }

  char filename[32];
  snprintf(filename, sizeof(filename), "/cfg_%s.bin", currentConfig.modelName);

  if (LittleFS.exists(filename)) {
    if (mbox_save_overwrite && lv_obj_is_valid(mbox_save_overwrite)) {
      lv_msgbox_close(mbox_save_overwrite);
    }
    mbox_save_overwrite = NULL;

    mbox_save_overwrite = lv_msgbox_create(tab_configs);
    if (!mbox_save_overwrite) return;

    lv_msgbox_add_title(mbox_save_overwrite, "Datei ueberschreiben?");
    char buf[64];
    snprintf(buf, sizeof(buf), "Das Modell '%s' existiert bereits.\nWirklich ueberschreiben?", currentConfig.modelName);
    lv_msgbox_add_text(mbox_save_overwrite, buf);

    lv_obj_t *b_ja = lv_msgbox_add_footer_button(mbox_save_overwrite, "Ja");
    lv_obj_t *b_nein = lv_msgbox_add_footer_button(mbox_save_overwrite, "Nein");

    if (b_ja && b_nein) {
      lv_obj_set_style_bg_color(b_ja, lv_palette_main(LV_PALETTE_RED), 0);
      lv_obj_add_event_cb(b_ja, mbox_save_overwrite_cb, LV_EVENT_CLICKED, NULL);
      lv_obj_add_event_cb(b_nein, mbox_save_overwrite_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_center(mbox_save_overwrite);
  } else {
    perform_local_save();
  }
}

/**
 * @brief Callback zum Senden der Konfiguration per ESP-NOW an den Empfaenger.
 */
static void btn_esp_send_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_PRESSED) {
    if (!is_config_changed) return;
    is_holding_send_btn = true;
    send_btn_press_start_ms = millis();
    trigger_audio_feedback(AUDIO_EVENT_CLICK);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'Rcv Senden'-Button gedrueckt - Halte-Timer gestartet...");
  }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (is_holding_send_btn) {
      unsigned long hold_duration = millis() - send_btn_press_start_ms;
      is_holding_send_btn = false;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
      SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'Rcv Senden'-Button losgelassen nach %lu ms.\n", hold_duration);
  }
    }
  }
}

/**
 * @brief Callback zum Anfordern der Konfiguration vom Empfaenger.
 */
static void btn_esp_request_cb(lv_event_t *e) {
  (void)e;
  ESPNowTransferMessage req;
  memset(&req, 0, sizeof(req));
  req.packetType = MSG_TYPE_CONFIG_REQ;
  req.chunkIndex = 0;
  req.totalChunks = 0;
  req.dataLength = 0;

  const uint8_t *targetMac = has_valid_active_paired_slot()
                             ? pairedModels[activeModelIndex].macAddress
                             : receiverMAC;

  esp_now_send(targetMac, (const uint8_t *)&req, sizeof(req));
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Konfiguration angefordert von %02X:%02X:%02X:%02X:%02X:%02X...\n",
                targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5]);
  }
}

/**
 * @brief Callback fuer Ueberschreiben bei eingehender Konfiguration.
 */
static void mbox_overwrite_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));

    if (btn_txt && strcmp(btn_txt, "Ja") == 0) {
      currentConfig = pendingIncomingConfig;
      saveConfigToFlash(currentConfig);
      store_config_into_active_paired_model(pendingIncomingConfig);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
      SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Datei ueberschrieben, geladen und im aktiven pairedModel gesichert.");
  }
      build_channel_list_page();
    }

    if (mbox_overwrite && lv_obj_is_valid(mbox_overwrite)) {
      lv_msgbox_close(mbox_overwrite);
    }
    mbox_overwrite = NULL;
  }
}

/**
 * @brief Callback fuer die Bestaetigung des Loeschens einer Modell-Config-Datei.
 */
static void mbox_delete_confirm_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));

    if (btn_txt && strcmp(btn_txt, "Loeschen") == 0) {
      char filename[32];
      snprintf(filename, sizeof(filename), "/cfg_%s.bin", currentConfig.modelName);

      if (LittleFS.exists(filename)) {
        LittleFS.remove(filename);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
        SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Config-Datei erfolgreich aus Flash geloescht.");
  }

        snprintf(currentConfig.modelName, MAX_NAME_LEN, "NeuesModell");
        currentConfig.channelCount = 2;
        lastSavedConfig = currentConfig;
        is_config_changed = false;
        check_config_changes();
        build_channel_list_page();
      }
    }

    if (mbox_delete_confirm && lv_obj_is_valid(mbox_delete_confirm)) {
      lv_msgbox_close(mbox_delete_confirm);
    }
    mbox_delete_confirm = NULL;
  }
}

/**
 * @brief Trigger-Button fuer das Loeschen der Config (nur aktiv, wenn kein Modell verbunden ist).
 */
static void btn_flash_delete_trigger_cb(lv_event_t *e) {
  (void)e;
  if (is_any_paired_model_connected()) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'Config loeschen' blockiert: Ein Modell ist aktuell verbunden.");
  }
    return;
  }

  mbox_delete_confirm = lv_msgbox_create(lv_screen_active());
  lv_msgbox_add_title(mbox_delete_confirm, "Config loeschen?");

  char buf[64];
  snprintf(buf, sizeof(buf), "Config '%s' wirklich unwiderruflich loeschen?", currentConfig.modelName);
  lv_msgbox_add_text(mbox_delete_confirm, buf);

  lv_obj_t *b_del = lv_msgbox_add_footer_button(mbox_delete_confirm, "Loeschen");
  lv_obj_set_style_bg_color(b_del, lv_palette_main(LV_PALETTE_RED), 0);

  lv_obj_t *b_can = lv_msgbox_add_footer_button(mbox_delete_confirm, "Abbrechen");

  lv_obj_add_event_cb(b_del, mbox_delete_confirm_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(b_can, mbox_delete_confirm_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_center(mbox_delete_confirm);
}

/**
 * @brief Callback fuer die Speicher-Warnung Messagebox.
 */
static void mbox_save_overwrite_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));

    if (btn_txt && strcmp(btn_txt, "Ja") == 0) {
      perform_local_save();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
      SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Modell-Datei nach Bestaetigung ueberschrieben.");
  }
    }

    if (mbox_save_overwrite && lv_obj_is_valid(mbox_save_overwrite)) {
      lv_msgbox_close(mbox_save_overwrite);
    }
    mbox_save_overwrite = NULL;
  }
}

/**
 * @brief Callback bei Aenderung des Modellnamens im Textfeld.
 */
static void ta_model_name_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && ta_model_name && lv_obj_is_valid(ta_model_name)) {
    snprintf(currentConfig.modelName, MAX_NAME_LEN, "%s", lv_textarea_get_text(ta_model_name));
    check_config_changes();
  }
}

/**
 * @brief Hilfs-Callback zum Abbrechen des Ladevorgangs.
 */
static void file_load_cancel_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    if (file_list_popup && lv_obj_is_valid(file_list_popup)) {
      lv_obj_delete(file_list_popup);
    }
    file_list_popup = NULL;
    lv_obj_t *bg_overlay = (lv_obj_t *)lv_event_get_current_target(e);
    if (bg_overlay && lv_obj_is_valid(bg_overlay)) {
      lv_obj_delete(bg_overlay);
    }
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Ladevorgang durch Klick ausserhalb abgebrochen.");
  }
  }
}

// Forward Declaration fuer Dateiauswahl
static void file_select_cb(lv_event_t *e);

/**
 * @brief Laedt eine Config aus dem Filesystem; bei verbundenem Modell wird sie direkt
 *        in pairedModels[idx].config geschrieben.
 * @param modelName Modellname aus der Dateiliste.
 */
static void apply_loaded_config_target(const char *modelName) {
  ModelConfig loadedCfg;
  memset(&loadedCfg, 0, sizeof(loadedCfg));
  if (!loadConfigFromFlash(modelName, loadedCfg)) {
    return;
  }

  int conn_idx = get_connected_paired_model_index();
  if (conn_idx >= 0) {
    pairedModels[conn_idx].config = loadedCfg;
    currentConfig = loadedCfg;
    lastSavedConfig = loadedCfg;
    is_config_changed = false;
    check_config_changes();
    mark_paired_model_config_dirty(conn_idx, true);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'lokal laden' bei verbundenem Modell: '%s' direkt in pairedModels[%d].config geschrieben.\n",
                  modelName, conn_idx);
  }
  } else {
    currentConfig = loadedCfg;
    lastSavedConfig = loadedCfg;
    is_config_changed = false;
    check_config_changes();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] 'lokal laden' ohne Verbindung: '%s' in currentConfig geladen.\n", modelName);
  }
  }
}

/**
 * @brief Callback zum Oeffnen der Lade-Auswahlliste.
 */
static void btn_flash_load_cb(lv_event_t *e) {
  (void)e;
  lv_obj_t *bg_overlay = lv_obj_create(lv_screen_active());
  lv_obj_set_size(bg_overlay, 480, 320);
  lv_obj_align(bg_overlay, LV_ALIGN_CENTER, 0, 0);

  lv_obj_set_style_bg_opa(bg_overlay, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bg_overlay, 0, 0);
  lv_obj_set_style_pad_all(bg_overlay, 0, 0);
  lv_obj_set_clickable(bg_overlay, true);

  lv_obj_add_event_cb(bg_overlay, file_load_cancel_cb, LV_EVENT_CLICKED, NULL);

  file_list_popup = lv_obj_create(bg_overlay);
  lv_obj_set_size(file_list_popup, 260, 200);
  lv_obj_center(file_list_popup);
  lv_obj_set_flex_flow(file_list_popup, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(file_list_popup, 4, 0);
  lv_obj_set_style_pad_row(file_list_popup, 4, 0);
  lv_obj_set_clickable(file_list_popup, true);

  fs::File root = LittleFS.open("/");
  int file_count = 0;

  if (root) {
    fs::File file = root.openNextFile();
    while (file) {
      const char *name = file.name();
      if (name && strncmp(name, "cfg_", 4) == 0) {
        char clean_name[MAX_NAME_LEN];
        snprintf(clean_name, sizeof(clean_name), "%s", name + 4);
        char *dot = strchr(clean_name, '.');
        if (dot) *dot = '\0';

        lv_obj_t *btn = lv_button_create(file_list_popup);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 36);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, LV_SYMBOL_FILE);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, clean_name);
        lv_obj_add_event_cb(btn, file_select_cb, LV_EVENT_CLICKED, NULL);
        file_count++;
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
  }

  if (file_count == 0) {
    lv_obj_t *btn = lv_button_create(file_list_popup);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 36);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "Keine Modelle");
    lv_obj_center(label);
    lv_obj_add_event_cb(
      btn, [](lv_event_t *ev) {
        if (file_list_popup && lv_obj_is_valid(file_list_popup)) {
          lv_obj_delete(file_list_popup);
        }
        file_list_popup = NULL;
        lv_obj_t *list_btn = (lv_obj_t *)lv_event_get_target(ev);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(list_btn)));
      },
      LV_EVENT_CLICKED, NULL);
  }
}

/**
 * @brief Callback fuer die Warnung beim Laden.
 */
static void mbox_load_warning_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));
    bool proceed_loading = (btn_txt && strcmp(btn_txt, "Ja") == 0);

    if (mbox_load_warning && lv_obj_is_valid(mbox_load_warning)) {
      lv_msgbox_close(mbox_load_warning);
    }
    mbox_load_warning = NULL;

    if (proceed_loading && strlen(pending_load_model_name) > 0) {
      apply_loaded_config_target(pending_load_model_name);
      build_channel_list_page();
    }
  }
}

/**
 * @brief Callback bei Auswahl einer Modelldatei.
 */
static void file_select_cb(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t *label = lv_obj_get_child(btn, 1);
  const char *model_name =
    label ? lv_label_get_text(label) : NULL;

  if (model_name) {
    snprintf(pending_load_model_name, sizeof(pending_load_model_name), "%s", model_name);

    if (is_config_changed) {
      if (file_list_popup && lv_obj_is_valid(file_list_popup)) {
        lv_obj_t *bg_overlay = lv_obj_get_parent(file_list_popup);
        if (bg_overlay && lv_obj_is_valid(bg_overlay)) lv_obj_delete(bg_overlay);
      }
      file_list_popup = NULL;

      if (mbox_load_warning && lv_obj_is_valid(mbox_load_warning)) {
        lv_msgbox_close(mbox_load_warning);
      }
      mbox_load_warning = NULL;

      mbox_load_warning = lv_msgbox_create(tab_configs);
      if (!mbox_load_warning) return;

      lv_msgbox_add_title(mbox_load_warning, "Aenderungen verwerfen?");

      char buf[128];
      snprintf(buf, sizeof(buf), "Das aktuelle Modell hat ungespeicherte Aenderungen.\nMoechtest du '%s' wirklich laden?", pending_load_model_name);
      lv_msgbox_add_text(mbox_load_warning, buf);

      lv_obj_t *b_ja = lv_msgbox_add_footer_button(mbox_load_warning, "Ja");
      lv_obj_t *b_nein = lv_msgbox_add_footer_button(mbox_load_warning, "Nein");

      if (b_ja && b_nein) {
        lv_obj_set_style_bg_color(b_ja, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_add_event_cb(b_ja, mbox_load_warning_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(b_nein, mbox_load_warning_cb, LV_EVENT_CLICKED, NULL);
      }

      lv_obj_center(mbox_load_warning);
      return;
    } else {
      apply_loaded_config_target(pending_load_model_name);
    }
  }

  if (file_list_popup && lv_obj_is_valid(file_list_popup)) {
    lv_obj_t *bg_overlay = lv_obj_get_parent(file_list_popup);
    if (bg_overlay && lv_obj_is_valid(bg_overlay)) lv_obj_delete(bg_overlay);
  }
  file_list_popup = NULL;
  build_channel_list_page();
}
#pragma endregion

#pragma region MODEL_PAGE_BUILD_AND_RUNTIME
/**
 * @brief Behandelt empfangene Konfigurationslogik bei ESP-NOW Empfang.
 *        Wenn das aktive gepairte Modell noch keine Config hat (channelCount <= 0),
 *        wird die Config direkt in pairedModels[activeModelIndex].config uebernommen
 *        und persistent gespeichert.
 */
void handle_received_config_logic() {
  log_model_page_state("handle_received_config_logic:ENTER");
  bool activePairedEmpty = (has_valid_active_paired_slot() &&
                            pairedModels[activeModelIndex].config.channelCount <= 0);

  if (activePairedEmpty) {
    store_config_into_active_paired_model(pendingIncomingConfig);
    currentConfig = pendingIncomingConfig;
    lastSavedConfig = currentConfig;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Leere pairedModels[%d].config direkt mit Receiver-Config '%s' (%d Ch) befuellt und gesichert.\n",
                  (int)activeModelIndex,
                  pendingIncomingConfig.modelName,
                  (int)pendingIncomingConfig.channelCount);
  }
    if (channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
      build_channel_list_page();
    }
    return;
  }

  if (has_valid_active_paired_slot()) {
    store_config_into_active_paired_model(pendingIncomingConfig);
  }

  char filename[32];
  snprintf(filename, sizeof(filename), "/cfg_%s.bin", pendingIncomingConfig.modelName);

  if (LittleFS.exists(filename)) {
    if (mbox_overwrite && lv_obj_is_valid(mbox_overwrite)) {
      lv_msgbox_close(mbox_overwrite);
    }
    mbox_overwrite = NULL;

    mbox_overwrite = lv_msgbox_create(tab_configs);
    if (!mbox_overwrite) return;

    lv_msgbox_add_title(mbox_overwrite, "Existiert bereits");
    lv_msgbox_add_text(mbox_overwrite, "Soll die vorhandene Modellkonfiguration ueberschrieben werden?");

    lv_obj_t *btn_ja = lv_msgbox_add_footer_button(mbox_overwrite, "Ja");
    lv_obj_t *btn_nein = lv_msgbox_add_footer_button(mbox_overwrite, "Nein");

    if (btn_ja && btn_nein) {
      lv_obj_add_event_cb(btn_ja, mbox_overwrite_cb, LV_EVENT_CLICKED, NULL);
      lv_obj_add_event_cb(btn_nein, mbox_overwrite_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_center(mbox_overwrite);
  } else {
    currentConfig = pendingIncomingConfig;
    saveConfigToFlash(currentConfig);
    build_channel_list_page();
    lastSavedConfig = currentConfig;
  }
}

/**
 * @brief Ueberprueft, ob Konfigurationsaenderungen vorliegen und aktualisiert den Tab "Configs *" dynamisch.
 *        Gibt Debug-Meldungen ausschliesslich bei Zustandsaenderung aus.
 */
void check_config_changes(void) {
  static int last_logged_changed_state = -1;
  int deviation = memcmp(&currentConfig, &lastSavedConfig, sizeof(ModelConfig));
  bool changed = (deviation != 0);

  if (changed != is_config_changed) {
    is_config_changed = changed;
  }
  update_config_action_buttons();

  if (tab_configs && lv_obj_is_valid(tab_configs)) {
    lv_obj_t *tab_content_area = lv_obj_get_parent(tab_configs);
    if (tab_content_area) {
      lv_obj_t *tabview = lv_obj_get_parent(tab_content_area);
      if (tabview) {
        lv_obj_t *tab_bar = lv_tabview_get_tab_bar(tabview);
        if (tab_bar) {
          uint32_t child_cnt = lv_obj_get_child_count(tab_bar);
          for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(tab_bar, i);
            if (!btn) continue;
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (!lbl) continue;
            const char *txt = lv_label_get_text(lbl);
            if (txt && (strncmp(txt, "Configs", 7) == 0)) {
              const char *desired_txt = is_config_changed ? "Configs *" : "Configs";
              if (strcmp(txt, desired_txt) != 0) {
                lv_label_set_text(lbl, desired_txt);
                lv_obj_invalidate(lbl);
              }
              int state_now = is_config_changed ? 1 : 0;
              if (state_now != last_logged_changed_state) {
                last_logged_changed_state = state_now;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
                SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Tab 'Configs' Markierung aktualisiert -> %s\n", desired_txt);
  }
              }
              break;
            }
          }
        }
      }
    }
  }
}

// Externer Event-Handler fuer Textareas
extern void ta_event_cb(lv_event_t *e);

/**
 * @brief Erstellt die Kanallisten-Seite im Setup-Tab. Zeigt bei verbundenem Modell
 *        stets die Kanalliste des aktuell verbundenen Modells.
 */
void build_channel_list_page(void) {
  log_model_page_state("build_channel_list_page:ENTER");
  if (!tab_configs || !lv_obj_is_valid(tab_configs)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][ERROR] build_channel_list_page abgebrochen: tab_configs ungueltig!");
    
  }
    return;
  }

  is_config_page_open = false;

  int8_t model_now = activeModelIndex;
  bool conn_now = is_any_paired_model_connected();
  if (model_now != last_tracked_model_idx || conn_now != last_tracked_connected) {
    sync_current_config_with_connected_model(true);
  } else {
    sync_current_config_with_connected_model(false);
  }
  last_tracked_model_idx = model_now;
  last_tracked_connected = conn_now;

  // Kanalanzahl sicher begrenzen
  if (currentConfig.channelCount < 0) currentConfig.channelCount = 0;
  if (currentConfig.channelCount > MAX_CHANNELS) currentConfig.channelCount = MAX_CHANNELS;

  if (channel_config_subpage && lv_obj_is_valid(channel_config_subpage)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] Loesche alte channel_config_subpage=%p\n", (void *)channel_config_subpage);
    
  }
    lv_obj_delete(channel_config_subpage);
  }
  channel_config_subpage = NULL;

  if (channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] Loesche alte channel_list_subpage=%p\n", (void *)channel_list_subpage);
    
  }
    lv_obj_delete(channel_list_subpage);
  }
  channel_list_subpage = NULL;

  ta_model_name = NULL;
  btn_send_ref = NULL;
  btn_save_ref = NULL;

  channel_list_subpage = lv_obj_create(tab_configs);
  if (!channel_list_subpage) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][ERROR] lv_obj_create(tab_configs) fuer channel_list_subpage lieferte NULL!");
    
  }
    return;
  }
  lv_obj_add_event_cb(channel_list_subpage, channel_list_subpage_deleted_cb, LV_EVENT_DELETE, NULL);

  lv_obj_set_size(channel_list_subpage, LV_PCT(100), LV_PCT(100));
  lv_obj_set_scrollable(channel_list_subpage, false);
  lv_obj_set_style_pad_all(tab_configs, 0, 0);
  lv_obj_set_style_pad_all(channel_list_subpage, 0, 0);
  lv_obj_set_style_pad_gap(channel_list_subpage, 0, 0);
  lv_obj_set_style_radius(channel_list_subpage, 0, 0);

  static int32_t main_col_dsc[] = { 166, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  static int32_t main_row_dsc[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  lv_obj_set_grid_dsc_array(channel_list_subpage, main_col_dsc, main_row_dsc);

  // LINKS: Fixierte Leiste
  lv_obj_t *btn_panel = lv_obj_create(channel_list_subpage);
  lv_obj_set_grid_cell(btn_panel, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
  lv_obj_set_flex_flow(btn_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(btn_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(btn_panel, 0, 0);
  lv_obj_set_style_pad_left(btn_panel, 4, 0);
  lv_obj_set_style_pad_right(btn_panel, 4, 0);
  lv_obj_set_style_border_width(btn_panel, 0, 0);
  lv_obj_set_style_bg_opa(btn_panel, LV_OPA_TRANSP, 0);

  lv_obj_t *btn_save = lv_button_create(btn_panel);
  lv_obj_set_size(btn_save, 156, 30);
  btn_save_ref = btn_save;
  lv_obj_add_event_cb(btn_save, btn_flash_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_save = lv_label_create(btn_save);
  lv_label_set_text(lbl_save, LV_SYMBOL_SAVE " lokal sichern");
  lv_obj_center(lbl_save);

  lv_obj_t *btn_load = lv_button_create(btn_panel);
  lv_obj_set_size(btn_load, 156, 30);
  lv_obj_add_event_cb(btn_load, btn_flash_load_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_load = lv_label_create(btn_load);
  lv_label_set_text(lbl_load, LV_SYMBOL_DIRECTORY " lokal laden");
  lv_obj_center(lbl_load);

  lv_obj_t *btn_send = lv_button_create(btn_panel);
  lv_obj_set_size(btn_send, 156, 30);
  btn_send_ref = btn_send;
  lv_obj_add_event_cb(btn_send, btn_esp_send_event_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(btn_send, btn_esp_send_event_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(btn_send, btn_esp_send_event_cb, LV_EVENT_PRESS_LOST, NULL);
  lv_obj_t *lbl_send = lv_label_create(btn_send);
  lv_label_set_text(lbl_send, LV_SYMBOL_DOWNLOAD " Rcv senden");
  lv_obj_center(lbl_send);

  lv_obj_t *btn_req = lv_button_create(btn_panel);
  lv_obj_set_size(btn_req, 156, 30);
  lv_obj_add_event_cb(btn_req, btn_esp_request_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_req = lv_label_create(btn_req);
  lv_label_set_text(lbl_req, LV_SYMBOL_UPLOAD " Rcv holen");
  lv_obj_center(lbl_req);

  lv_obj_t *btn_delete = lv_button_create(btn_panel);
  lv_obj_set_size(btn_delete, 156, 30);
  lv_obj_add_event_cb(btn_delete, btn_flash_delete_trigger_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_delete = lv_label_create(btn_delete);
  lv_label_set_text(lbl_delete, LV_SYMBOL_TRASH " Config loeschen");
  lv_obj_center(lbl_delete);

  // "Config loeschen" nur aktiv, wenn kein Modell verbunden ist
  if (last_tracked_connected) {
    lv_obj_add_state(btn_delete, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(btn_delete, lv_palette_main(LV_PALETTE_GREY), 0);
  } else {
    lv_obj_clear_state(btn_delete, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(btn_delete, lv_palette_main(LV_PALETTE_RED), 0);
  }

  lv_obj_t *add_btn = lv_button_create(btn_panel);
  lv_obj_set_size(add_btn, 156, 30);
  lv_obj_add_event_cb(add_btn, add_channel_cb, LV_EVENT_CLICKED, NULL);
  if (currentConfig.channelCount >= MAX_CHANNELS) {
    lv_obj_add_state(add_btn, LV_STATE_DISABLED);
  }
  lv_obj_t *add_lbl = lv_label_create(add_btn);
  lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " neuer Kanal");
  lv_obj_center(add_lbl);

  // RECHTS: Kanalliste
  lv_obj_t *right_panel = lv_obj_create(channel_list_subpage);
  lv_obj_set_grid_cell(right_panel, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
  lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(right_panel, 4, 0);
  lv_obj_set_style_pad_row(right_panel, 4, 0);
  lv_obj_set_style_border_width(right_panel, 0, 0);
  lv_obj_set_style_bg_opa(right_panel, LV_OPA_TRANSP, 0);

  lv_obj_t *cont_name = lv_obj_create(right_panel);
  lv_obj_set_size(cont_name, LV_PCT(100), 36);
  lv_obj_set_flex_flow(cont_name, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(cont_name, 0, 0);
  lv_obj_set_style_border_width(cont_name, 0, 0);
  lv_obj_set_style_bg_opa(cont_name, LV_OPA_TRANSP, 0);

  lv_obj_t *lbl_m_name = lv_label_create(cont_name);
  lv_label_set_text(lbl_m_name, "Modellname:");
  lv_obj_set_style_pad_top(lbl_m_name, 12, 0);

  ta_model_name = lv_textarea_create(cont_name);
  lv_textarea_set_one_line(ta_model_name, true);
  lv_textarea_set_text(ta_model_name, currentConfig.modelName);
  lv_obj_set_width(ta_model_name, 190);
  lv_obj_set_style_text_font(ta_model_name, &lv_font_montserrat_14, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_text_font(ta_model_name, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_font(ta_model_name, &lv_font_montserrat_14, LV_PART_CURSOR);
  lv_obj_add_event_cb(ta_model_name, ta_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(ta_model_name, ta_model_name_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *list = lv_obj_create(right_panel);
  lv_obj_set_size(list, LV_PCT(100), 140);
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 4, 0);
  lv_obj_set_style_pad_row(list, 4, 0);
  lv_obj_set_scrollable(list, true);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  for (intptr_t i = 0; i < (intptr_t)currentConfig.channelCount; i++) {
    char buf[40];
    snprintf(buf, sizeof(buf), "CH %d: %s [%s]", (int)(i + 1), currentConfig.channels[i].name,
             currentConfig.channels[i].type == TYPE_ANALOG ? "Analog" : "Digital");
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 3, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_size(btn, 0, 32);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, buf);
    lv_obj_add_event_cb(btn, channel_click_cb, LV_EVENT_CLICKED, (void *)i);

    lv_obj_t *btn_up = lv_button_create(row);
    lv_obj_set_size(btn_up, 28, 32);
    lv_obj_add_event_cb(btn_up, channel_move_up_cb, LV_EVENT_CLICKED, (void *)i);
    lv_obj_t *lbl_up = lv_label_create(btn_up);
    lv_label_set_text(lbl_up, LV_SYMBOL_UP);
    lv_obj_center(lbl_up);

    lv_obj_t *btn_down = lv_button_create(row);
    lv_obj_set_size(btn_down, 28, 32);
    lv_obj_add_event_cb(btn_down, channel_move_down_cb, LV_EVENT_CLICKED, (void *)i);
    lv_obj_t *lbl_down = lv_label_create(btn_down);
    lv_label_set_text(lbl_down, LV_SYMBOL_DOWN);
    lv_obj_center(lbl_down);
  }
  check_config_changes();

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
  SerialInterface::printDebug(SerialInterface::Module::Models, "[DEBUG] Kanallisten-Seite aufgebaut (verbunden=%s, Modell-Idx=%d, Modell='%s', Kanaele=%d, list_sub=%p).\n",
                last_tracked_connected ? "JA" : "NEIN",
                (int)activeModelIndex,
                currentConfig.modelName,
                (int)currentConfig.channelCount,
                (void *)channel_list_subpage);
  
  }
  print_vram_status_model("Kanallisten-Seite Aufbau");
}

/**
 * @brief Prueft periodisch Modellwechsel, Verbindungsstatus und den Konfig.-senden-Button.
 */
void update_model_page_ui(void) {
  bool validActiveSlot = has_valid_active_paired_slot();
  bool connectedNow = is_any_paired_model_connected();

  // Bei Wechsel des aktiven Modells oder Wechsel des Verbindungszustands Kanalliste aktualisieren
  if ((activeModelIndex != last_tracked_model_idx) || (connectedNow != last_tracked_connected)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[MODEL_PAGE][TRACE] update_model_page_ui: Zustandswechsel erkannt (idx: %d->%d, conn: %d->%d, chOpen=%d)\n",
                  (int)last_tracked_model_idx, (int)activeModelIndex,
                  (int)last_tracked_connected, (int)connectedNow,
                  (int)is_config_page_open);
    
  }
    last_tracked_model_idx = activeModelIndex;
    last_tracked_connected = connectedNow;
    if (!is_config_page_open && channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
      build_channel_list_page();
    }
  }

  if (is_holding_send_btn) {
    unsigned long held_time = millis() - send_btn_press_start_ms;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
    SerialInterface::printDebug(SerialInterface::Module::Models, "[UI] Rcv Senden gedrueckt fuer %lu ms.", held_time);
  }
    static unsigned long last_continuous_send_ms = 0;
    if (millis() - last_continuous_send_ms >= 600) {
      last_continuous_send_ms = millis();
      trigger_async_model_config_send(currentConfig, receiverMAC);
    }

    if (validActiveSlot &&
        pairedModels[activeModelIndex].telemetry.configReceived == 0 &&
        !is_success_feedback_active &&
        btn_send_ref != NULL &&
        lv_obj_is_valid(btn_send_ref)) {
      if (held_time < 5000) {
        lv_obj_set_style_bg_color(btn_send_ref, lv_palette_main(LV_PALETTE_ORANGE), 0);
      } else {
        lv_obj_set_style_bg_color(btn_send_ref, lv_palette_darken(LV_PALETTE_ORANGE, 1), 0);
      }
    }
  } else {
    if (!is_success_feedback_active) {
      update_config_action_button(btn_send_ref);
    }
  }

  if (validActiveSlot &&
      pairedModels[activeModelIndex].telemetry.configReceived == 1 &&
      !is_success_feedback_active) {
    is_success_feedback_active = true;
    success_feedback_start_ms = millis();

    if (btn_send_ref != NULL && lv_obj_is_valid(btn_send_ref)) {
      lv_obj_set_style_bg_color(btn_send_ref, lv_palette_main(LV_PALETTE_GREEN), 0);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
      SerialInterface::printDebug(SerialInterface::Module::Models, "[UI] Konfiguration erfolgreich uebertragen! Button 3s gruen eingefaerbt.");
  }
    }
  }

  if (is_success_feedback_active) {
    if (millis() - success_feedback_start_ms >= 3000 &&
        (!validActiveSlot || pairedModels[activeModelIndex].telemetry.configReceived == 0)) {
      is_success_feedback_active = false;
      if (btn_send_ref != NULL && lv_obj_is_valid(btn_send_ref)) {
        update_config_action_button(btn_send_ref);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Models)) {
        SerialInterface::printDebug(SerialInterface::Module::Models, "[UI] Sende-Button zur Aenderungsanzeige zurueckgesetzt.");
  }
      }
    }
  }
}
#pragma endregion