/**
 * @file models_page.cpp
 * @version 1.2.0
 * @brief Implementierung des Haupt-Tabs "Modelle" mit Speichern-Button je Modellzeile
 *        (nur aktiv bei ungespeicherten Aenderungen an pairedModels[idx].config) und
 *        Verwaltung/Priorisierung gepairter Empfaenger.
 * @Version_info v1.2.0: Zusaetzlichen Speichern-Button in jedem Listeneintrag ergaenzt, der nur
 *               aktiv ist, wenn ungespeicherte Aenderungen (is_paired_model_config_dirty) vorliegen;
 *               beim Auswaehlen eines Modells wird request_model_switch_with_guard() genutzt.
 *               v1.1.1: Ergaenzung lueckenloser Doxygen- und Zeilenkommentare.
 */


#include <Arduino.h>
#include <lvgl.h>
#include "models_page.h"
#include "model_page.h"
#include "pairing_manager.h"
#include "espnow_comm.h"
#include "SerialInterface.h"
#include "audio_module.h"

#pragma region GLOBALS_AND_STATICS
/** @brief Zeiger auf das uebergeordnete Tab-Container-Objekt der Modellseite. */
static lv_obj_t *tab_models_container = NULL;

/** @brief Label-Objekt zur dynamischen Anzeige des aktuellen Pairing-Status und der Restlaufzeit. */
static lv_obj_t *lbl_pairing_status = NULL;

/** @brief Button-Objekt zum manuellen Starten des 30-sekundaeren Pairing-Fensters. */
static lv_obj_t *btn_start_pairing = NULL;

/** @brief Scrollbarer Container zur Aufnahme der dynamischen Liste aller gepairten Modelle. */
static lv_obj_t *list_models_cont = NULL;

// Externe Referenzen auf den globalen Zustand aus espnow_comm.h und pairing_manager.h
extern PairedModelInfo *pairedModels;
extern int8_t pairedModelCount;
extern unsigned long pairingWindowStartTime; // Startzeitpunkt (Millis) des aktiven Pairing-Fensters
#pragma endregion

#pragma region PROTOTYPES
static void print_vram_status_models(const char *tag);
static void start_pairing_btn_cb(lv_event_t *e);
static void model_save_row_btn_cb(lv_event_t *e);
static void model_select_row_cb(lv_event_t *e);
static void model_up_btn_cb(lv_event_t *e);
static void model_down_btn_cb(lv_event_t *e);
static void model_delete_btn_cb(lv_event_t *e);
void refresh_models_list_ui(void);
#pragma endregion

#pragma region INTERNAL_HELPERS
/**
 * @brief Interne Diagnosefunktion zur Ueberwachung des LVGL VRAM-Speichers.
 * @param tag Bezeichner fuer den Aufrufkontext.
 */
static void print_vram_status_models(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Groesster freier Block: %u Bytes\n",
                tag, (int)mon.used_pct, (unsigned int)mon.free_biggest_size);
  }
}
#pragma endregion

#pragma region PAGE_BUILDER
/**
 * @brief Erstellt die gesamte grafische UI-Struktur des Modell-Tabs (Grid, Header, Listencontainer).
 * @param parent_tab Zeiger auf den uebergeordneten Tab-Container.
 */
void build_models_page(lv_obj_t *parent_tab) {
  // SCHRITT 1: Basissetting des Container-Tabs anwenden
  tab_models_container = parent_tab;
  lv_obj_set_scrollable(tab_models_container, false);
  lv_obj_set_style_pad_all(tab_models_container, 4, 0);
  lv_obj_set_style_border_width(tab_models_container, 0, 0);

  static int32_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  static int32_t row_dsc[] = { 45, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  lv_obj_set_grid_dsc_array(tab_models_container, col_dsc, row_dsc);

  // SCHRITT 2: Obere Aktionsleiste (Pairing-Startbutton und Statuslabel) erstellen
  lv_obj_t *header_cont = lv_obj_create(tab_models_container);
  lv_obj_set_grid_cell(header_cont, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
  lv_obj_set_flex_flow(header_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(header_cont, 4, 0);
  lv_obj_set_style_bg_color(header_cont, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_scrollable(header_cont, false);

  btn_start_pairing = lv_button_create(header_cont);
  lv_obj_set_size(btn_start_pairing, 160, 32);
  lv_obj_set_style_bg_color(btn_start_pairing, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_add_event_cb(btn_start_pairing, start_pairing_btn_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_btn = lv_label_create(btn_start_pairing);
  lv_label_set_text(lbl_btn, LV_SYMBOL_WIFI " Pairing (30s)");
  lv_obj_center(lbl_btn);

  lbl_pairing_status = lv_label_create(header_cont);
  lv_label_set_text(lbl_pairing_status, "Bereit");
  lv_obj_set_style_text_color(lbl_pairing_status, lv_color_white(), 0);

  // SCHRITT 3: Unterer Bereich (Scrollbare Liste der gepairten Modelle) erstellen
  list_models_cont = lv_obj_create(tab_models_container);
  lv_obj_set_grid_cell(list_models_cont, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
  lv_obj_set_flex_flow(list_models_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list_models_cont, 4, 0);
  lv_obj_set_style_pad_row(list_models_cont, 6, 0);
  lv_obj_set_scrollable(list_models_cont, true);

  refresh_models_list_ui();
  refresh_models_tab_star_indicator();
  print_vram_status_models("BUILD_MODELS_PAGE");
}
#pragma endregion

#pragma region UI_REFRESH_AND_CALLBACKS
/**
 * @brief Aktualisiert die Liste der gepairten Modelle auf der UI strikt sortiert nach sortOrder.
 */
void refresh_models_list_ui(void) {
  // SCHRITT 1: Basisschutz - Pruefen, ob der Container gueltig ist.
  if (!list_models_cont || !lv_obj_is_valid(list_models_cont)) return;

  // Vorhandene Listeneintraege vollstaendig bereinigen
  lv_obj_clean(list_models_cont);

  // SCHRITT 2: Erfasse alle gueltigen Indizes und sortiere sie nach sortOrder aufsteigend
  int sortedIndices[MAX_PAIRED_MODELS];
  int activeCount = 0;

  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired) {
      sortedIndices[activeCount++] = i;
    }
  }

  // Bubble-Sort Algorithmus zur Sortierung nach Prioritaet (sortOrder)
  for (int i = 0; i < activeCount - 1; i++) {
    for (int j = i + 1; j < activeCount; j++) {
      if (pairedModels[sortedIndices[j]].sortOrder < pairedModels[sortedIndices[i]].sortOrder) {
        int temp = sortedIndices[i];
        sortedIndices[i] = sortedIndices[j];
        sortedIndices[j] = temp;
      }
    }
  }

  // SCHRITT 3: Erstelle fuer jedes sortierte Modell eine grafische UI-Zeile
  for (int slot = 0; slot < activeCount; slot++) {
    int i = sortedIndices[slot];
    bool row_dirty = is_paired_model_config_dirty(i);

    lv_obj_t *item_row = lv_obj_create(list_models_cont);
    lv_obj_set_size(item_row, LV_PCT(100), 45);
    lv_obj_set_flex_flow(item_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(item_row, 2, 0);
    lv_obj_set_style_border_width(item_row, 0, 0);
    lv_obj_set_style_bg_color(item_row, lv_color_hex(0x222222), 0);
    lv_obj_set_scrollable(item_row, false);
    lv_obj_add_event_cb(item_row, model_select_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    // Formatierter Text mit Prioritaet, Modellname, Dirty-Stern und MAC-Adresse
    char info_buf[104];
    const char *display_name = pairedModels[i].config.modelName[0] != '\0'
                                 ? pairedModels[i].config.modelName
                                 : pairedModels[i].modelName;
    snprintf(info_buf, sizeof(info_buf), "[Prio %d] %s%s\nMAC: %02X:%02X:%02X:%02X:%02X:%02X",
             slot + 1,
             display_name,
             row_dirty ? " *" : "",
             pairedModels[i].macAddress[0], pairedModels[i].macAddress[1],
             pairedModels[i].macAddress[2], pairedModels[i].macAddress[3],
             pairedModels[i].macAddress[4], pairedModels[i].macAddress[5]);

    lv_obj_t *lbl_item = lv_label_create(item_row);
    lv_label_set_text(lbl_item, info_buf);
    lv_obj_set_width(lbl_item, 220);
    lv_obj_set_style_text_font(lbl_item, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_item, lv_color_white(), 0);

    // Container fuer die Aktions-Buttons rechts (Speichern, Hoch, Runter, Loeschen)
    lv_obj_t *btn_group = lv_obj_create(item_row);
    lv_obj_set_size(btn_group, 128, 38);
    lv_obj_set_flex_flow(btn_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_group, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn_group, 0, 0);
    lv_obj_set_style_pad_column(btn_group, 2, 0);
    lv_obj_set_style_border_width(btn_group, 0, 0);
    lv_obj_set_style_bg_opa(btn_group, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(btn_group, false);

    // Button: Speichern (nur aktiv, wenn ungespeicherte Aenderungen an pairedModels[i].config vorliegen)
    lv_obj_t *btn_save_row = lv_button_create(btn_group);
    lv_obj_set_size(btn_save_row, 28, 30);
    lv_obj_add_event_cb(btn_save_row, model_save_row_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    if (row_dirty) {
      lv_obj_clear_state(btn_save_row, LV_STATE_DISABLED);
      lv_obj_set_style_bg_color(btn_save_row, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
      lv_obj_add_state(btn_save_row, LV_STATE_DISABLED);
      lv_obj_set_style_bg_color(btn_save_row, lv_palette_main(LV_PALETTE_GREY), 0);
    }
    lv_obj_t *lbl_save_row = lv_label_create(btn_save_row);
    lv_label_set_text(lbl_save_row, LV_SYMBOL_SAVE);
    lv_obj_center(lbl_save_row);

    // Button: Prio Hoch (nach oben verschieben)
    lv_obj_t *btn_up = lv_button_create(btn_group);
    lv_obj_set_size(btn_up, 28, 30);
    lv_obj_add_event_cb(btn_up, model_up_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t *lbl_up = lv_label_create(btn_up);
    lv_label_set_text(lbl_up, LV_SYMBOL_UP);
    lv_obj_center(lbl_up);

    // Button: Prio Runter (nach unten verschieben)
    lv_obj_t *btn_down = lv_button_create(btn_group);
    lv_obj_set_size(btn_down, 28, 30);
    lv_obj_add_event_cb(btn_down, model_down_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t *lbl_down = lv_label_create(btn_down);
    lv_label_set_text(lbl_down, LV_SYMBOL_DOWN);
    lv_obj_center(lbl_down);

    // Button: Loeschen (Modell aus der Kopplung entfernen)
    lv_obj_t *btn_del = lv_button_create(btn_group);
    lv_obj_set_size(btn_del, 28, 30);
    lv_obj_set_style_bg_color(btn_del, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(btn_del, model_delete_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t *lbl_del = lv_label_create(btn_del);
    lv_label_set_text(lbl_del, LV_SYMBOL_TRASH);
    lv_obj_center(lbl_del);
  }
}

/**
 * @brief Event-Callback zum Speichern ungespeicherter Aenderungen an pairedModels[idx].config.
 * @param e Zeiger auf das LVGL-Ereignis.
 */
static void model_save_row_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (!is_paired_model_config_dirty(idx)) return;
  save_paired_model_config_by_index(idx);
  trigger_audio_feedback(AUDIO_EVENT_CLICK);
}

/**
 * @brief Event-Callback beim Klick auf einen Modell-Listeneintrag (Modellwechsel mit Guard).
 * @param e Zeiger auf das LVGL-Ereignis.
 */
static void model_select_row_cb(lv_event_t *e) {
  lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
  if (lv_obj_check_type(target, &lv_button_class)) return;
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  request_model_switch_with_guard(idx);
  trigger_audio_feedback(AUDIO_EVENT_CLICK);
}

/**
 * @brief Event-Callback zum Starten des 30-sekundaeren Pairing-Fensters.
 * @param e Zeiger auf das LVGL-Ereignis.
 */
static void start_pairing_btn_cb(lv_event_t *e) {
  start_pairing_window();
  trigger_audio_feedback(AUDIO_EVENT_SWITCH);
}

/**
 * @brief Event-Callback zum Erhoehen der Prioritaet (Modell in der Liste nach oben bewegen).
 * @param e Zeiger auf das LVGL-Ereignis.
 */
static void model_up_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  move_paired_model_order(idx, -1);
  refresh_models_list_ui();
  trigger_audio_feedback(AUDIO_EVENT_CLICK);
}

/**
 * @brief Event-Callback zum Verringern der Prioritaet (Modell in der Liste nach unten bewegen).
 * @param e Zeiger auf das LVGL-Ereignis.
 */
static void model_down_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  move_paired_model_order(idx, +1);
  refresh_models_list_ui();
  trigger_audio_feedback(AUDIO_EVENT_CLICK);
}

/**
 * @brief Event-Callback zum Loeschen eines gekoppelten Modells.
 * @param e Zeiger auf das LVGL-Ereignis.
 */
static void model_delete_btn_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  delete_paired_model(idx);
  mark_paired_model_config_dirty(idx, false);
  refresh_models_list_ui();
  trigger_audio_feedback(AUDIO_EVENT_WARNING);
}

/**
 * @brief Zyklische Aktualisierungsfunktion der Modellseite (berechnet und zeigt die exakte Restlaufzeit in Sekunden).
 */
void update_models_page_ui(void) {
  // SCHRITT 1: Basisschutz - Pruefen, ob das Statuslabel gueltig ist.
  if (!lbl_pairing_status || !lv_obj_is_valid(lbl_pairing_status)) return;

  // SCHRITT 2: Fallunterscheidung nach aktivem oder inaktivem Pairing-Fenster
  if (is_pairing_window_active()) {
    unsigned long elapsed = millis() - pairingWindowStartTime;
    int remainingSec = (int)((PAIRING_WINDOW_DURATION_MS - elapsed) / 1000);
    if (remainingSec < 0) remainingSec = 0;

    char statusBuf[32];
    snprintf(statusBuf, sizeof(statusBuf), "PAIRING (%ds)...", remainingSec);
    lv_label_set_text(lbl_pairing_status, statusBuf);
    lv_obj_set_style_text_color(lbl_pairing_status, lv_palette_main(LV_PALETTE_ORANGE), 0);
  } else {
    lv_label_set_text(lbl_pairing_status, "Bereit");
    lv_obj_set_style_text_color(lbl_pairing_status, lv_color_white(), 0);
  }
}

/**
 * @brief Bereinigt saemtliche UI-Ressourcen der Modellseite beim Verlassen des Tabs.
 */
void cleanup_models_page(void) {
  if (tab_models_container && lv_obj_is_valid(tab_models_container)) {
    lv_obj_clean(tab_models_container);
  }
  tab_models_container = NULL;
  lbl_pairing_status = NULL;
  btn_start_pairing = NULL;
  list_models_cont = NULL;
}
#pragma endregion