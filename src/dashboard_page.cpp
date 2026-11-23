/**
 * @file dashboard_page.cpp
 * @version 1.3.1
 * @brief Implementierung des Dashboard-Tabs ohne Fallback auf currentConfig bei verbundenem Modell,
 *        mit automatischer Config-Anforderung vom Receiver bei leerer pairedModels[activeModelIndex].config,
 *        identischem Neuaufbau via on_active_model_changed_ui_hook() bei Modellwechsel wie beim Tabwechsel
 *        und dynamischem Neuaufbau nach Erhalt.
 * @Version_info v1.3.1: on_active_model_changed_ui_hook() implementiert, sodass ein Modellwechsel ueber
 *               die Top-Bar denselben build_dashboard_tab()-Neuaufbau ausloest wie beim Tabwechsel;
 *               zusaetzlich Neuaufbau in update_dashboard_ui() bei aktivem Modellwechsel synchronisiert.
 *               v1.3.0: Fallback auf currentConfig bei verbundenem Modell entfernt; klare Statusanzeige bei leerer
 *               pairedModels[activeModelIndex].config; automatische MSG_TYPE_CONFIG_REQ-Anforderung und
 *               Neuaufbau sobald channelCount > 0 vorliegt.
 */


#include <lvgl.h>
#include <Arduino.h>
#include <esp_now.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <config_types.h>
#include "dashboard_page.h"
#include "espnow_comm.h"
#include "hardware_io.h"
#include "SerialInterface.h"

// Externe Deklarationen der globalen Modellkonfiguration und Multi-Modell-Verwaltung
extern ModelConfig currentConfig;
extern PairedModelInfo *pairedModels;
extern int8_t activeModelIndex;

// ============================================================
// GLOBALE UI-REFERENZEN UND ZUSTANDSVARIABLEN
// ============================================================

#pragma region DASHBOARD_STATE_AND_REFS

static portMUX_TYPE winchMux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *tab_dashboard = NULL;
static lv_obj_t *cont_gear_bar = NULL;
static lv_obj_t *btn_gear_mode = NULL;
static lv_obj_t *lbl_gear_mode = NULL;

// Sektionen & Dynamische Container
static lv_obj_t *cont_lighting_col = NULL;
static lv_obj_t *cont_right_col = NULL;
static lv_obj_t *lbl_pitch_val = NULL;
static lv_obj_t *lbl_roll_val = NULL;
static lv_obj_t *cont_winch_box = NULL;
static lv_obj_t *btn_winch_in = NULL;
static lv_obj_t *btn_winch_out = NULL;
static lv_obj_t *lbl_missing_cfg_status = NULL;

// Referenzen auf Beleuchtungs-Widgets zur Live-Spiegelung bei Hardware-Gebern
static lv_obj_t *light_widgets[MAX_CHANNELS] = { NULL };
static bool light_is_hw_driven[MAX_CHANNELS] = { false };

// Audio & Lautstaerke
static lv_obj_t *btn_audio_block = NULL;
static lv_obj_t *lbl_volume_val = NULL;
static lv_obj_t *popup_vol_win = NULL;
static lv_obj_t *lbl_popup_vol_level = NULL;

// Zustandsvariablen
static bool drivingAutoMode = true;               // true = Vollautomatik ("D"), false = Sequenziell ("M")
static uint8_t currentActiveGear = 2;             // Startgang
static uint8_t currentVolume = 3;                 // Lautstaerke Stufe 1-5
static volatile bool winchMasterEnabled = true;  // true = Sperrschalter aktiv (Kanal auf 1500 us fixiert)
static volatile bool winchMovingIn = false;       // Status Einrollen via Touch-Button
static volatile bool winchMovingOut = false;      // Status Ausrollen via Touch-Button
static unsigned long winchBlockFlashUntilMs = 0;  // Zeitstempel fuer 5-Hz-Rahmenblinken bei gesperrtem HW-Geber

// Zustandsmerker fuer Verbindungs-, Modell- und Config-Zustand waehrend der Anzeige
static bool lastConnectedState = false;
static int8_t lastObservedModelIdx = -127;
static int lastObservedPairedChCount = -1;
static unsigned long lastAutoConfigReqMs = 0;
static const unsigned long AUTO_CONFIG_REQ_INTERVAL_MS = 3000;

// Puffer fuer virtuelle Kanal-Rohwerte (Steuerung ueber UI-Schalter/Taster wie Lichter)
uint16_t virtual_channel_raw[MAX_CHANNELS] = { 1500 };

// Prototypen fuer Callbacks
static void gear_mode_switch_event_cb(lv_event_t *e);
static void volume_bar_event_cb(lv_event_t *e);
static void volume_slider_event_cb(lv_event_t *e);
static void close_popup_event_cb(lv_event_t *e);
static void winch_toggle_event_cb(lv_event_t *e);
static void winch_action_event_cb(lv_event_t *e);
static void light_control_event_cb(lv_event_t *e);
void rebuild_dynamic_gear_bar(void);

/**
 * @brief Liefert die Anzahl der Kanaele einer ModelConfig sicher begrenzt auf 0..MAX_CHANNELS.
 * @param cfg Konstanten Zeiger auf eine ModelConfig.
 * @return Gueltige Kanalanzahl im Bereich 0..MAX_CHANNELS.
 */
static int get_safe_channel_count(const ModelConfig *cfg) {
  if (!cfg) return 0;
  int cnt = (int)cfg->channelCount;
  if (cnt < 0) return 0;
  if (cnt > MAX_CHANNELS) return MAX_CHANNELS;
  return cnt;
}

/**
 * @brief Prueft, ob aktuell ein gepairtes Modell mit gueltigem Index aktiv/verbunden ist.
 * @return true, wenn pairedModels != NULL, activeModelIndex gueltig und isPaired == true ist.
 */
static bool has_active_paired_model(void) {
  return (pairedModels != NULL &&
          activeModelIndex >= 0 &&
          activeModelIndex < MAX_PAIRED_MODELS &&
          pairedModels[activeModelIndex].isPaired);
}

/**
 * @brief Liefert die aktuell massgebliche ModelConfig OHNE Fallback auf currentConfig,
 *        sobald ein gepairtes Modell aktiv ist.
 * @return Zeiger auf pairedModels[activeModelIndex].config bei aktivem gepairtem Modell,
 *         sonst &currentConfig (nur wenn kein gepairtes Modell aktiv ist).
 */
static const ModelConfig* get_active_dashboard_config(void) {
  if (has_active_paired_model()) {
    return &pairedModels[activeModelIndex].config;
  }
  return &currentConfig;
}

/**
 * @brief Sendet eine Konfigurationsanforderung (MSG_TYPE_CONFIG_REQ) an den aktuell aktiven Empfaenger.
 * @param forceNow Wenn true, wird das Intervall-Limit ignoriert.
 */
static void request_config_for_active_model(bool forceNow) {
  if (!has_active_paired_model()) return;

  unsigned long now_ms = millis();
  if (!forceNow && (now_ms - lastAutoConfigReqMs < AUTO_CONFIG_REQ_INTERVAL_MS)) {
    return;
  }
  lastAutoConfigReqMs = now_ms;

  ESPNowTransferMessage req;
  memset(&req, 0, sizeof(req));
  req.packetType = MSG_TYPE_CONFIG_REQ;
  req.chunkIndex = 0;
  req.totalChunks = 0;
  req.dataLength = 0;

  const uint8_t *targetMac = pairedModels[activeModelIndex].macAddress;
  esp_err_t res = esp_now_send(targetMac, (const uint8_t *)&req, sizeof(req));

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Config vom Receiver angefordert fuer Slot %d ('%s') -> MAC %02X:%02X:%02X:%02X:%02X:%02X (err=%d)\n",
                (int)activeModelIndex,
                pairedModels[activeModelIndex].modelName,
                targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5],
                (int)res);
  }
}

/**
 * @brief Prueft, ob in der aktiven Modellkonfiguration ein Kanal mit dem Namen "SOUND" existiert.
 * @return true, wenn der Kanal "SOUND" vorhanden ist, sonst false.
 */
static bool has_sound_channel(void) {
  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);

  for (int i = 0; i < ch_count; i++) {
    if (strcmp(cfg->channels[i].name, "SOUND") == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Aktualisiert die segmentierte Balken- und Stufendarstellung der Lautstaerke auf dem Audio-Button.
 *        Stellt aktive Stufen (1..currentVolume) mit gefuellten Segmenten ('#') und leere Stufen mit ('-') dar.
 */
static void refresh_volume_label(void) {
  if (!lbl_volume_val || !lv_obj_is_valid(lbl_volume_val)) return;

  if (currentVolume < 1) currentVolume = 1;
  if (currentVolume > 5) currentVolume = 5;

  char bar[8] = {0};
  for (int v = 1; v <= 5; v++) {
    bar[v - 1] = (v <= (int)currentVolume) ? '#' : '-';
  }
  bar[5] = '\0';

  char buf[48];
  snprintf(buf, sizeof(buf), "AUDIO: [%s] %u/5", bar, (unsigned int)currentVolume);
  lv_label_set_text(lbl_volume_val, buf);
}

/**
 * @brief Sucht den Kanalindex fuer den Kanalnamen "WINCH" in der aktiven Modellkonfiguration.
 * @return Kanalindex >= 0 oder -1, falls nicht vorhanden.
 */
static int find_winch_channel_index(void) {
  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);

  for (int i = 0; i < ch_count; i++) {
    if (strcmp(cfg->channels[i].name, "WINCH") == 0) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Gibt den aktuellen VRAM-Speicherstatus von LVGL im seriellen Monitor aus.
 * @param tag Bezeichner fuer die Debug-Ausgabe.
 */
static void print_vram_status(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  uint32_t largest_free = mon.free_biggest_size;
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Groesster freier Block: %u Bytes\n",
                tag,
                (int)mon.used_pct,
                (unsigned int)largest_free);
  }
}

/**
 * @brief Ermittelt den aktuellen Schaltzustand (AN/AUS) eines hardware-gesteuerten Kanals.
 * @param ch Referenz auf die Kanaldefinition.
 * @return true, wenn der Geber als aktiv/EIN gilt, sonst false.
 */
static bool read_hardware_light_state(const ChannelConfig &ch) {
  uint8_t hw_id = ch.hardwareInputChannel;
  if (hw_id == 0) return false;

  // Analoge Geber (1..4): Auslenkung ueber 50% der kalibrierten Spanne als EIN werten
  if (hw_id >= 1 && hw_id <= 4) {
    int16_t raw_val = read_hardware_raw(hw_id);
    int16_t pct = read_hardware_calibrated_percent(hw_id, raw_val, ch.deadband);
    bool active = (pct > 50);
    return ch.invert ? !active : active;
  }

  // Digitale/sonstige Hardware-Geber: Rohwert pruefen
  int16_t dig_raw = read_hardware_raw(hw_id);
  bool active = (dig_raw != 0);
  return ch.invert ? !active : active;
}

bool is_winch_locked(void) {
  bool locked = false;
  portENTER_CRITICAL(&winchMux);
  locked = winchMasterEnabled;
  portEXIT_CRITICAL(&winchMux);
  return locked;
}

uint16_t get_winch_button_target_pwm(int winch_channel_idx) {
  bool locked = false;
  bool move_in = false;
  bool move_out = false;

  portENTER_CRITICAL(&winchMux);
  locked = winchMasterEnabled;
  move_in = winchMovingIn;
  move_out = winchMovingOut;
  portEXIT_CRITICAL(&winchMux);

  if (locked) return 1500;

  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);
  if (!cfg || winch_channel_idx < 0 || winch_channel_idx >= ch_count) {
    return 1500;
  }

  if (!move_in && !move_out) {
    return 1500;
  }

  const ChannelConfig &ch = cfg->channels[winch_channel_idx];
  float target_pct = 0.0f;

  if (move_in) {
    // 50% des definierten Einroll-Limits (constrainMin)
    float min_limit = (ch.constrainMin != 0) ? (float)ch.constrainMin : -100.0f;
    target_pct = min_limit * 0.5f;
  } else if (move_out) {
    // 50% des definierten Ausroll-Limits (constrainMax)
    float max_limit = (ch.constrainMax != 0) ? (float)ch.constrainMax : 100.0f;
    target_pct = max_limit * 0.5f;
  }

  // Umrechnung in PWM (1500 us Mitte, 1% = 5 us) mit Begrenzung auf 1000..2000 us
  int pwm = 1500 + (int)(target_pct * 5.0f);
  if (pwm < 1000) pwm = 1000;
  if (pwm > 2000) pwm = 2000;
  return (uint16_t)pwm;
}

uint16_t get_winch_hardware_pwm(int winch_channel_idx) {
  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);
  if (!cfg || winch_channel_idx < 0 || winch_channel_idx >= ch_count) {
    return 1500;
  }

  const ChannelConfig &ch = cfg->channels[winch_channel_idx];
  uint8_t hw_id = ch.hardwareInputChannel;
  if (hw_id == 0) {
    return 1500;
  }

  int pct = 0;
  if (hw_id >= 1 && hw_id <= 4) {
    int16_t raw_val = read_hardware_raw(hw_id);
    pct = (int)read_hardware_calibrated_percent(hw_id, raw_val, ch.deadband);
  } else {
    int16_t dig_raw = read_hardware_raw(hw_id);
    pct = (dig_raw != 0) ? 100 : 0;
  }

  // Totbereich (deadband) beruecksichtigen
  if (abs(pct) <= (int)ch.deadband) {
    return 1500;
  }

  float f_pct = (float)pct;
  if (ch.invert) {
    f_pct = -f_pct;
  }

  // Richtungsbezogenes Limit (constrainMin / constrainMax) anwenden
  if (f_pct < 0.0f) {
    float min_limit = (ch.constrainMin != 0) ? (float)abs(ch.constrainMin) : 100.0f;
    f_pct = -(abs(f_pct) * (min_limit / 100.0f));
  } else if (f_pct > 0.0f) {
    float max_limit = (ch.constrainMax != 0) ? (float)abs(ch.constrainMax) : 100.0f;
    f_pct = f_pct * (max_limit / 100.0f);
  }

  int pwm = 1500 + (int)(f_pct * 5.0f);
  if (pwm < 1000) pwm = 1000;
  if (pwm > 2000) pwm = 2000;
  return (uint16_t)pwm;
}

/**
 * @brief Hook bei Modellwechsel aus espnow_comm.cpp: loest denselben Neuaufbau
 *        von build_dashboard_tab() wie beim Tabwechsel aus, sofern der Dashboard-Tab aktiv ist.
 * @param newModelIndex Neuer aktiver Modell-Index (-1 = kein Modell aktiv).
 */
void on_active_model_changed_ui_hook(int newModelIndex) {
  if (!tab_dashboard || !lv_obj_is_valid(tab_dashboard)) {
    return;
  }
  if (lv_obj_is_hidden(tab_dashboard)) {
    return;
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] on_active_model_changed_ui_hook(%d) -> Identischer Neuaufbau von build_dashboard_tab().\n",
                newModelIndex);
  }
  build_dashboard_tab(tab_dashboard);
}

#pragma endregion

// ============================================================
// DASHBOARD ERSTELLUNG (480x320 Pixel, strikt ohne Scrollen, Radius 0, minimales Padding)
// ============================================================

#pragma region DASHBOARD_BUILD

void build_dashboard_tab(lv_obj_t *parent_tab) {
  if (!parent_tab || !lv_obj_is_valid(parent_tab)) return;

  tab_dashboard = parent_tab;
  lv_obj_clean(tab_dashboard);
  lv_obj_set_scrollable(tab_dashboard, false);
  lv_obj_set_style_pad_all(tab_dashboard, 1, 0);
  lv_obj_set_style_border_width(tab_dashboard, 0, 0);
  lv_obj_set_style_radius(tab_dashboard, 0, 0);

  // Zeiger vor dem Neuaufbau sauber zuruecksetzen
  cont_gear_bar = NULL;
  btn_gear_mode = NULL;
  lbl_gear_mode = NULL;
  cont_lighting_col = NULL;
  cont_right_col = NULL;
  lbl_pitch_val = NULL;
  lbl_roll_val = NULL;
  cont_winch_box = NULL;
  btn_winch_in = NULL;
  btn_winch_out = NULL;
  lbl_missing_cfg_status = NULL;
  btn_audio_block = NULL;
  lbl_volume_val = NULL;

  bool pairedActive = has_active_paired_model();
  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);
  bool pairedConfigEmpty = (pairedActive && ch_count <= 0);

  lastObservedModelIdx = activeModelIndex;
  lastObservedPairedChCount = pairedActive ? ch_count : -1;
  lastConnectedState = (hasTelemetry && ((millis() - lastTelemetryTime) < 2000));

  if (pairedConfigEmpty) {
    request_config_for_active_model(true);
  }

  if (hasTelemetry && incomingTelemetry.liveDataReceived == 1) {
    currentActiveGear = incomingTelemetry.currentGear;
  }

  // Haupt-Layout: Spalten-Layout (Kopfzeile oben, Hauptbereich unten)
  lv_obj_set_flex_flow(tab_dashboard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab_dashboard, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tab_dashboard, 1, 0);

  // ------------------------------------------------------------
  // 1. KOPFZEILE: Geteilt (Ganganzeige links, Modus rechts) - Fixe Hoehe 34px
  // ------------------------------------------------------------
  lv_obj_t *cont_header = lv_obj_create(tab_dashboard);
  lv_obj_set_size(cont_header, 474, 34);
  lv_obj_set_flex_flow(cont_header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont_header, 1, 0);
  lv_obj_set_style_border_width(cont_header, 1, 0);
  lv_obj_set_style_border_color(cont_header, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_radius(cont_header, 0, 0);
  lv_obj_set_scrollable(cont_header, false);

  // Linke Spalte: Ganganzeige
  cont_gear_bar = lv_obj_create(cont_header);
  lv_obj_set_size(cont_gear_bar, 420, 30);
  lv_obj_set_flex_flow(cont_gear_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_gear_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont_gear_bar, 0, 0);
  lv_obj_set_style_pad_column(cont_gear_bar, 2, 0);
  lv_obj_set_style_border_width(cont_gear_bar, 0, 0);
  lv_obj_set_style_bg_opa(cont_gear_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(cont_gear_bar, 0, 0);
  lv_obj_set_scrollable(cont_gear_bar, false);

  rebuild_dynamic_gear_bar();

  // Rechte Spalte: Getriebemodus-Schalter ("D" / "M")
  btn_gear_mode = lv_button_create(cont_header);
  lv_obj_set_size(btn_gear_mode, 46, 30);
  lv_obj_set_style_radius(btn_gear_mode, 0, 0);
  lv_obj_set_style_bg_color(btn_gear_mode, lv_color_hex(0xADD8E6), 0);
  lv_obj_set_style_border_width(btn_gear_mode, 1, 0);
  lv_obj_set_style_border_color(btn_gear_mode, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_add_event_cb(btn_gear_mode, gear_mode_switch_event_cb, LV_EVENT_CLICKED, NULL);

  lbl_gear_mode = lv_label_create(btn_gear_mode);
  lv_label_set_text(lbl_gear_mode, drivingAutoMode ? "D" : "M");
  lv_obj_center(lbl_gear_mode);
  lv_obj_set_style_text_font(lbl_gear_mode, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl_gear_mode, lv_color_black(), 0);

  // ------------------------------------------------------------
  // 2. HAUPTBEREICH UNTERHALB DER KOPFZEILE (Exakte Hoehe 218px, absolut ueberlappungsfrei)
  // ------------------------------------------------------------
  lv_obj_t *cont_main_grid = lv_obj_create(tab_dashboard);
  lv_obj_set_size(cont_main_grid, 474, 218);
  lv_obj_set_flex_flow(cont_main_grid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_main_grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont_main_grid, 1, 0);
  lv_obj_set_style_border_width(cont_main_grid, 0, 0);
  lv_obj_set_style_bg_opa(cont_main_grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(cont_main_grid, 0, 0);
  lv_obj_set_scrollable(cont_main_grid, false);

  // --- LINKE HAELFTE: Beleuchtung ---
  cont_lighting_col = lv_obj_create(cont_main_grid);
  lv_obj_set_size(cont_lighting_col, 234, 214);
  lv_obj_set_style_radius(cont_lighting_col, 0, 0);
  lv_obj_set_flex_flow(cont_lighting_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(cont_lighting_col, 2, 0);
  lv_obj_set_style_pad_row(cont_lighting_col, 1, 0);
  lv_obj_set_style_border_width(cont_lighting_col, 1, 0);
  lv_obj_set_style_border_color(cont_lighting_col, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_scrollable(cont_lighting_col, false);

  lv_obj_t *lbl_light_hdr = lv_label_create(cont_lighting_col);
  lv_label_set_text(lbl_light_hdr, "BELEUCHTUNG & SIGNALE");
  lv_obj_set_style_text_font(lbl_light_hdr, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_light_hdr, lv_palette_main(LV_PALETTE_YELLOW), 0);

  // Arrays vor dem dynamischen Aufbau initialisieren
  for (int i = 0; i < MAX_CHANNELS; i++) {
    light_widgets[i] = NULL;
    light_is_hw_driven[i] = false;
  }

  if (pairedConfigEmpty) {
    lbl_missing_cfg_status = lv_label_create(cont_lighting_col);
    lv_label_set_text(lbl_missing_cfg_status, "Keine Modell-Config geladen!\nFordere Config vom Receiver an...");
    lv_obj_set_style_text_font(lbl_missing_cfg_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_missing_cfg_status, lv_palette_main(LV_PALETTE_RED), 0);
  } else {
    // Dynamisches Generieren der Beleuchtungs-Widgets aus der aktiven Modellkonfiguration
    bool lightFound = false;
    if (cfg) {
      for (int i = 0; i < ch_count; i++) {
        const ChannelConfig &ch = cfg->channels[i];
        if (strncmp(ch.name, "LIGHT_", 6) == 0) {
          lightFound = true;
          const char *label_text = ch.name + 6;

          lv_obj_t *row = lv_obj_create(cont_lighting_col);
          lv_obj_set_size(row, 224, 24);
          lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
          lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
          lv_obj_set_style_pad_all(row, 0, 0);
          lv_obj_set_style_border_width(row, 0, 0);
          lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
          lv_obj_set_style_radius(row, 0, 0);
          lv_obj_set_scrollable(row, false);

          lv_obj_t *lbl_item = lv_label_create(row);
          lv_label_set_text(lbl_item, label_text);
          lv_obj_set_style_text_font(lbl_item, &lv_font_montserrat_12, 0);

          if (ch.hardwareInputChannel == 0) {
            light_is_hw_driven[i] = false;
            if (ch.behaviour == BEHAVIOUR_TOGGLE) {
              lv_obj_t *sw = lv_switch_create(row);
              lv_obj_set_size(sw, 36, 18);
              lv_obj_add_event_cb(sw, light_control_event_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
              light_widgets[i] = sw;
            } else {
              lv_obj_t *btn = lv_button_create(row);
              lv_obj_set_size(btn, 50, 20);
              lv_obj_set_style_radius(btn, 0, 0);
              lv_obj_t *lbl_btn = lv_label_create(btn);
              lv_label_set_text(lbl_btn, "AN");
              lv_obj_center(lbl_btn);
              lv_obj_add_event_cb(btn, light_control_event_cb, LV_EVENT_ALL, (void*)(intptr_t)i);
              light_widgets[i] = btn;
            }
          } else {
            // Hardware-Geber aktiv: reine Statusanzeige (AN/AUS) via deaktiviertem Switch
            light_is_hw_driven[i] = true;
            lv_obj_t *sw_status = lv_switch_create(row);
            lv_obj_set_size(sw_status, 36, 18);
            lv_obj_add_state(sw_status, LV_STATE_DISABLED);
            light_widgets[i] = sw_status;
          }
        }
      }
    }

    if (!lightFound) {
      lv_obj_t *lbl_no_light = lv_label_create(cont_lighting_col);
      lv_label_set_text(lbl_no_light, "Keine LIGHT_ Kanaele definiert.");
      lv_obj_set_style_text_font(lbl_no_light, &lv_font_montserrat_10, 0);
    }
  }

  // --- RECHTE HAELFTE: Oben Pitch/Roll Boxen, Mitte Winde, Unten Audio ---
  cont_right_col = lv_obj_create(cont_main_grid);
  lv_obj_set_size(cont_right_col, 234, 214);
  lv_obj_set_style_radius(cont_right_col, 0, 0);
  lv_obj_set_flex_flow(cont_right_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(cont_right_col, 2, 0);
  lv_obj_set_style_pad_row(cont_right_col, 3, 0);
  lv_obj_set_style_border_width(cont_right_col, 1, 0);
  lv_obj_set_style_border_color(cont_right_col, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_scrollable(cont_right_col, false);

  // 1. Oben: Getrennte Attitude-Boxen mit kontrastreichem schwarzen Text
  lv_obj_t *attitude_box_pitch = lv_obj_create(cont_right_col);
  lv_obj_set_size(attitude_box_pitch, 222, 22);
  lv_obj_set_style_radius(attitude_box_pitch, 0, 0);
  lv_obj_set_style_pad_all(attitude_box_pitch, 1, 0);
  lv_obj_set_style_bg_color(attitude_box_pitch, lv_color_hex(0xD0F0D0), 0);
  lv_obj_set_scrollable(attitude_box_pitch, false);

  lbl_pitch_val = lv_label_create(attitude_box_pitch);
  lv_label_set_text(lbl_pitch_val, "Nick: --- deg");
  lv_obj_set_style_text_font(lbl_pitch_val, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_pitch_val, lv_color_black(), 0);
  lv_obj_center(lbl_pitch_val);

  lv_obj_t *attitude_box_roll = lv_obj_create(cont_right_col);
  lv_obj_set_size(attitude_box_roll, 222, 22);
  lv_obj_set_style_radius(attitude_box_roll, 0, 0);
  lv_obj_set_style_pad_all(attitude_box_roll, 1, 0);
  lv_obj_set_style_bg_color(attitude_box_roll, lv_color_hex(0xD0F0D0), 0);
  lv_obj_set_scrollable(attitude_box_roll, false);

  lbl_roll_val = lv_label_create(attitude_box_roll);
  lv_label_set_text(lbl_roll_val, "Roll: --- deg");
  lv_obj_set_style_text_font(lbl_roll_val, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_roll_val, lv_color_black(), 0);
  lv_obj_center(lbl_roll_val);

  // 2. Mitte: Windensteuerung (Nur anzeigen, wenn Kanal "WINCH" existiert)
  int winch_idx = (!pairedConfigEmpty) ? find_winch_channel_index() : -1;
  if (winch_idx >= 0) {
    cont_winch_box = lv_obj_create(cont_right_col);
    lv_obj_set_size(cont_winch_box, 222, 78);
    lv_obj_set_style_radius(cont_winch_box, 0, 0);
    lv_obj_set_style_border_width(cont_winch_box, 1, 0);
    lv_obj_set_style_border_color(cont_winch_box, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_flex_flow(cont_winch_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont_winch_box, 2, 0);
    lv_obj_set_style_pad_row(cont_winch_box, 2, 0);
    lv_obj_set_scrollable(cont_winch_box, false);

    lv_obj_t *winch_top_row = lv_obj_create(cont_winch_box);
    lv_obj_set_size(winch_top_row, 214, 22);
    lv_obj_set_flex_flow(winch_top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(winch_top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(winch_top_row, 0, 0);
    lv_obj_set_style_border_width(winch_top_row, 0, 0);
    lv_obj_set_style_bg_opa(winch_top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(winch_top_row, 0, 0);
    lv_obj_set_scrollable(winch_top_row, false);

    lv_obj_t *lbl_winch_lbl = lv_label_create(winch_top_row);
    lv_label_set_text(lbl_winch_lbl, "WINDE (Sperre):");
    lv_obj_set_style_text_font(lbl_winch_lbl, &lv_font_montserrat_12, 0);

    lv_obj_t *sw_winch_master = lv_switch_create(winch_top_row);
    lv_obj_set_size(sw_winch_master, 36, 18);
    if (is_winch_locked()) {
      lv_obj_add_state(sw_winch_master, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw_winch_master, winch_toggle_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *winch_btn_row = lv_obj_create(cont_winch_box);
    lv_obj_set_size(winch_btn_row, 214, 30);
    lv_obj_set_flex_flow(winch_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(winch_btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(winch_btn_row, 0, 0);
    lv_obj_set_style_border_width(winch_btn_row, 0, 0);
    lv_obj_set_style_bg_opa(winch_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(winch_btn_row, 0, 0);
    lv_obj_set_scrollable(winch_btn_row, false);

    // Bei Sperre (winchMasterEnabled == true) grau, sonst blau
    lv_color_t initial_winch_color = is_winch_locked() ? lv_palette_main(LV_PALETTE_GREY) : lv_palette_main(LV_PALETTE_BLUE);

    btn_winch_in = lv_button_create(winch_btn_row);
    lv_obj_set_size(btn_winch_in, 100, 26);
    lv_obj_set_style_radius(btn_winch_in, 0, 0);
    lv_obj_set_style_bg_color(btn_winch_in, initial_winch_color, 0);
    lv_obj_add_event_cb(btn_winch_in, winch_action_event_cb, LV_EVENT_ALL, (void*)1);
    lv_obj_t *lbl_wi = lv_label_create(btn_winch_in);
    lv_label_set_text(lbl_wi, "Einrollen");
    lv_obj_center(lbl_wi);
    lv_obj_set_style_text_font(lbl_wi, &lv_font_montserrat_12, 0);

    btn_winch_out = lv_button_create(winch_btn_row);
    lv_obj_set_size(btn_winch_out, 100, 26);
    lv_obj_set_style_radius(btn_winch_out, 0, 0);
    lv_obj_set_style_bg_color(btn_winch_out, initial_winch_color, 0);
    lv_obj_add_event_cb(btn_winch_out, winch_action_event_cb, LV_EVENT_ALL, (void*)2);
    lv_obj_t *lbl_wo = lv_label_create(btn_winch_out);
    lv_label_set_text(lbl_wo, "Ausrollen");
    lv_obj_center(lbl_wo);
    lv_obj_set_style_text_font(lbl_wo, &lv_font_montserrat_12, 0);
  }

  // 3. Unten: Lautstaerke / Audio (dynamisch bei vorhandenem SOUND-Kanal)
  btn_audio_block = lv_button_create(cont_right_col);
  lv_obj_set_size(btn_audio_block, 222, 38);
  lv_obj_set_style_radius(btn_audio_block, 0, 0);
  lv_obj_set_style_bg_color(btn_audio_block, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(btn_audio_block, 1, 0);
  lv_obj_set_style_border_color(btn_audio_block, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_add_event_cb(btn_audio_block, volume_bar_event_cb, LV_EVENT_CLICKED, NULL);

  lbl_volume_val = lv_label_create(btn_audio_block);
  lv_obj_center(lbl_volume_val);
  lv_obj_set_style_text_font(lbl_volume_val, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_volume_val, lv_palette_main(LV_PALETTE_BLUE), 0);
  refresh_volume_label();

  if (pairedConfigEmpty || !has_sound_channel()) {
    lv_obj_set_hidden(btn_audio_block, true);
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Dashboard-Tab (Version 1.3.1) aufgebaut. Quelle: %s (idx=%d) | Modell: '%s' | Kanaele: %d | Leer: %s\n",
                pairedActive ? "PAIRED_MODEL_CONFIG" : "CURRENT_CONFIG",
                (int)activeModelIndex,
                cfg ? cfg->modelName : "NONE",
                ch_count,
                pairedConfigEmpty ? "JA" : "NEIN");
  }
  print_vram_status("Dashboard-Tab Aufbau");
}

#pragma endregion

// ============================================================
// CALLBACKS UND LAUFZEIT-AKTUALISIERUNG
// ============================================================

#pragma region DASHBOARD_CALLBACKS_AND_RUNTIME

static void gear_mode_switch_event_cb(lv_event_t *e) {
  drivingAutoMode = !drivingAutoMode;
  if (drivingAutoMode) {
    lv_label_set_text(lbl_gear_mode, "D");
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Getriebemodus: Vollautomatik (D)");
  }
  } else {
    lv_label_set_text(lbl_gear_mode, "M");
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Getriebemodus: Sequenziell (M)");
  }
  }
}

static void winch_toggle_event_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

  portENTER_CRITICAL(&winchMux);
  winchMasterEnabled = checked;
  if (winchMasterEnabled) {
    winchMovingIn = false;
    winchMovingOut = false;
  }
  portEXIT_CRITICAL(&winchMux);

  lv_color_t target_color = checked ? lv_palette_main(LV_PALETTE_GREY) : lv_palette_main(LV_PALETTE_BLUE);
  if (btn_winch_in && lv_obj_is_valid(btn_winch_in)) {
    lv_obj_set_style_bg_color(btn_winch_in, target_color, 0);
  }
  if (btn_winch_out && lv_obj_is_valid(btn_winch_out)) {
    lv_obj_set_style_bg_color(btn_winch_out, target_color, 0);
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Winden-Sperrschalter: %s (Kanal auf 1500 us %s)\n",
                checked ? "GESPERRT" : "FREIGEGEBEN",
                checked ? "fixiert" : "freigegeben");
  }
}

static void winch_action_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  int action_type = (int)(intptr_t)lv_event_get_user_data(e);

  if (is_winch_locked()) {
    portENTER_CRITICAL(&winchMux);
    winchMovingIn = false;
    winchMovingOut = false;
    portEXIT_CRITICAL(&winchMux);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 1000) {
      SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Windenaktion blockiert: Winde ist gesperrt (1500 us).");
      lastDebug = millis();
    }
  }
    return;
  }

  if (code == LV_EVENT_PRESSED) {
    portENTER_CRITICAL(&winchMux);
    if (action_type == 1) {
      winchMovingIn = true;
      winchMovingOut = false;
    } else if (action_type == 2) {
      winchMovingOut = true;
      winchMovingIn = false;
    }
    portEXIT_CRITICAL(&winchMux);

    if (btn && lv_obj_is_valid(btn)) {
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_YELLOW), 0);
    }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Winde Button aktiv: %s (50%% Richtungs-Limit)\n",
                  action_type == 1 ? "Einrollen" : "Ausrollen");
  }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    portENTER_CRITICAL(&winchMux);
    winchMovingIn = false;
    winchMovingOut = false;
    portEXIT_CRITICAL(&winchMux);

    if (btn && lv_obj_is_valid(btn)) {
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Winde Button losgelassen.");
  }
  }
}

/**
 * @brief Steuert virtuelle Beleuchtungskanaele (hw_id == 0) fuer Toggle-Schalter und Momentary-Taster.
 * @param e LVGL-Event mit Kanalindex als user_data.
 */
static void light_control_event_cb(lv_event_t *e) {
  int channel_idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (channel_idx < 0 || channel_idx >= MAX_CHANNELS) return;

  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
  bool updated = false;
  bool state = false;

  // Toggle-Schalter (lv_switch) ueber LV_EVENT_VALUE_CHANGED auswerten
  if (lv_obj_check_type(target, &lv_switch_class)) {
    if (code == LV_EVENT_VALUE_CHANGED) {
      state = lv_obj_has_state(target, LV_STATE_CHECKED);
      virtual_channel_raw[channel_idx] = state ? 2000 : 1000;
      updated = true;
    }
  } else {
    // Momentary-Taster (lv_button): EIN beim Druecken, AUS beim Loslassen / Verlassen
    if (code == LV_EVENT_PRESSED) {
      state = true;
      virtual_channel_raw[channel_idx] = 2000;
      lv_obj_set_style_bg_color(target, lv_palette_main(LV_PALETTE_YELLOW), 0);
      updated = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
      state = false;
      virtual_channel_raw[channel_idx] = 1000;
      lv_obj_set_style_bg_color(target, lv_palette_main(LV_PALETTE_BLUE), 0);
      updated = true;
    }
  }

  if (updated) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    const char *ch_name = (cfg && channel_idx < ch_count) ? cfg->channels[channel_idx].name : "UNBEKANNT";
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Licht Kanal %d ('%s') aktualisiert: %s (Rohwert: %u us)\n",
                  channel_idx + 1,
                  ch_name,
                  state ? "EIN" : "AUS",
                  (unsigned int)virtual_channel_raw[channel_idx]);
  }
  }
}

/**
 * @brief Aktualisiert die Lautstaerke bei Bewegung des Sliders im Audio-Popup.
 * @param e LVGL-Event des Lautstaerke-Sliders.
 */
static void volume_slider_event_cb(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  if (!slider || !lv_obj_is_valid(slider)) return;

  int32_t val = lv_slider_get_value(slider);
  if (val < 1) val = 1;
  if (val > 5) val = 5;
  currentVolume = (uint8_t)val;

  refresh_volume_label();

  if (lbl_popup_vol_level && lv_obj_is_valid(lbl_popup_vol_level)) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Stufe: %u / 5", (unsigned int)currentVolume);
    lv_label_set_text(lbl_popup_vol_level, buf);
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Dashboard Audio-Lautstaerke geaendert auf Stufe: %u / 5\n", (unsigned int)currentVolume);
  }
}

static void volume_bar_event_cb(lv_event_t *e) {
  if (!has_sound_channel()) return;
  if (popup_vol_win != NULL) return;

  popup_vol_win = lv_obj_create(lv_screen_active());
  lv_obj_set_size(popup_vol_win, 300, 175);
  lv_obj_center(popup_vol_win);
  lv_obj_set_style_radius(popup_vol_win, 0, 0);
  lv_obj_set_style_border_width(popup_vol_win, 2, 0);
  lv_obj_set_style_border_color(popup_vol_win, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_set_style_bg_color(popup_vol_win, lv_color_hex(0x111111), 0);
  lv_obj_set_flex_flow(popup_vol_win, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(popup_vol_win, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(popup_vol_win, 8, 0);
  lv_obj_set_style_pad_row(popup_vol_win, 6, 0);
  lv_obj_set_scrollable(popup_vol_win, false);

  lv_obj_t *lbl_popup_title = lv_label_create(popup_vol_win);
  lv_label_set_text(lbl_popup_title, "LAUTSTAERKE (SOUND)");
  lv_obj_set_style_text_font(lbl_popup_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_popup_title, lv_palette_main(LV_PALETTE_YELLOW), 0);

  lbl_popup_vol_level = lv_label_create(popup_vol_win);
  char lvl_buf[32];
  snprintf(lvl_buf, sizeof(lvl_buf), "Stufe: %u / 5", (unsigned int)currentVolume);
  lv_label_set_text(lbl_popup_vol_level, lvl_buf);
  lv_obj_set_style_text_font(lbl_popup_vol_level, &lv_font_montserrat_14, 0);

  lv_obj_t *slider_vol = lv_slider_create(popup_vol_win);
  lv_obj_set_width(slider_vol, 260);
  lv_slider_set_range(slider_vol, 1, 5);
  lv_slider_set_value(slider_vol, currentVolume, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider_vol, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *btn_close = lv_button_create(popup_vol_win);
  lv_obj_set_size(btn_close, 260, 34);
  lv_obj_set_style_radius(btn_close, 0, 0);
  lv_obj_add_event_cb(btn_close, close_popup_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lbl_close = lv_label_create(btn_close);
  lv_label_set_text(lbl_close, "SCHLIESSEN [X]");
  lv_obj_center(lbl_close);
}

static void close_popup_event_cb(lv_event_t *e) {
  if (popup_vol_win != NULL) {
    lv_obj_delete(popup_vol_win);
    popup_vol_win = NULL;
    lbl_popup_vol_level = NULL;
  }
}

/**
 * @brief Aktualisiert periodisch die Dashboard-Werte im Haupt-Loop (Beleuchtungsstatus, Pitch/Roll, Winde, Audio, Gaenge).
 */
void update_dashboard_ui(void) {
  if (!lv_obj_is_valid(tab_dashboard)) {
    return;
  }
  unsigned long now_ms = millis();
  bool pairedActive = has_active_paired_model();
  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);
  int pairedChCount = pairedActive ? ch_count : -1;
  bool isConnected = (hasTelemetry && ((now_ms - lastTelemetryTime) < 2000));

  // Wenn pairedModels[activeModelIndex].config frisch mit Kanaelen befuellt wurde oder
  // das aktive Modell gewechselt hat, Dashboard komplett via build_dashboard_tab() neu aufbauen
  if ((pairedActive && pairedChCount > 0 && lastObservedPairedChCount <= 0) ||
      (activeModelIndex != lastObservedModelIdx)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Modell- oder Config-Wechsel erkannt (Modell-Idx: %d -> %d, Kanaele: %d -> %d) -> Neuaufbau via build_dashboard_tab().\n",
                  (int)lastObservedModelIdx,
                  (int)activeModelIndex,
                  lastObservedPairedChCount,
                  pairedChCount);
  }
    build_dashboard_tab(tab_dashboard);
    return;
  }

  // Verbindungswechsel waehrend der Tab-Anzeige erkennen und synchronisieren
  if (isConnected != lastConnectedState) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Dashboard Verbindungszustand geaendert -> Verbunden: %s (vorher %s)\n",
                  isConnected ? "JA" : "NEIN",
                  lastConnectedState ? "JA" : "NEIN");
  }
    lastConnectedState = isConnected;
    rebuild_dynamic_gear_bar();
  }

  bool pairedConfigEmpty = (pairedActive && ch_count <= 0);
  if (pairedConfigEmpty) {
    if (lbl_missing_cfg_status && lv_obj_is_valid(lbl_missing_cfg_status)) {
      lv_label_set_text(lbl_missing_cfg_status,
                        isConnected ? "Config leer -> Fordere vom Receiver an..."
                                    : "Config leer (0 Kanaele) -> Warte auf Link/Config");
    }
    if (isConnected) {
      request_config_for_active_model(false);
    }
  }

  // Audio-Block dynamisch ein-/ausblenden, abhaengig vom Kanal "SOUND"
  if (btn_audio_block && lv_obj_is_valid(btn_audio_block)) {
    if (!pairedConfigEmpty && has_sound_channel()) {
      lv_obj_set_hidden(btn_audio_block, false);
      refresh_volume_label();
    } else {
      lv_obj_set_hidden(btn_audio_block, true);
      if (popup_vol_win != NULL) {
        lv_obj_delete(popup_vol_win);
        popup_vol_win = NULL;
        lbl_popup_vol_level = NULL;
      }
    }
  }

  // Hardware-gesteuerte LIGHT_-Kanaele (Statusanzeige AN/AUS via deaktiviertem Switch) spiegeln
  if (cfg && !pairedConfigEmpty) {
    for (int i = 0; i < ch_count; i++) {
      if (light_is_hw_driven[i] && light_widgets[i] != NULL && lv_obj_is_valid(light_widgets[i])) {
        bool hw_on = read_hardware_light_state(cfg->channels[i]);
        if (hw_on) {
          lv_obj_add_state(light_widgets[i], LV_STATE_CHECKED);
        } else {
          lv_obj_clear_state(light_widgets[i], LV_STATE_CHECKED);
        }
      }
    }
  }

  // Winden-Zustand, Hardware-IO-Feedback sowie 5-Hz-Blockade-Blinken aktualisieren
  int winch_idx = (!pairedConfigEmpty) ? find_winch_channel_index() : -1;
  if (winch_idx >= 0 && cont_winch_box && lv_obj_is_valid(cont_winch_box) &&
      btn_winch_in && btn_winch_out && lv_obj_is_valid(btn_winch_in) && lv_obj_is_valid(btn_winch_out)) {

    uint16_t hw_pwm = get_winch_hardware_pwm(winch_idx);
    int hw_dev = (int)hw_pwm - 1500;
    bool hw_moving_in = (hw_dev < 0);
    bool hw_moving_out = (hw_dev > 0);
    bool locked = is_winch_locked();

    bool move_in_btn = false;
    bool move_out_btn = false;
    portENTER_CRITICAL(&winchMux);
    move_in_btn = winchMovingIn;
    move_out_btn = winchMovingOut;
    portEXIT_CRITICAL(&winchMux);

    if (locked) {
      // Sperre aktiv: Falls Hardware-IO aus der Neutralstellung (inkl. Deadband) bewegt wird, Blinkfenster starten
      if (hw_dev != 0) {
        winchBlockFlashUntilMs = now_ms + 500;
      }

      if (now_ms < winchBlockFlashUntilMs) {
        bool blink_red_phase = ((now_ms % 500UL) < 250UL);
        if (blink_red_phase) {
          lv_obj_set_style_border_width(cont_winch_box, 2, 0);
          lv_obj_set_style_border_color(cont_winch_box, lv_palette_main(LV_PALETTE_RED), 0);
        } else {
          lv_obj_set_style_border_width(cont_winch_box, 1, 0);
          lv_obj_set_style_border_color(cont_winch_box, lv_palette_main(LV_PALETTE_GREY), 0);
        }
      } else {
        lv_obj_set_style_border_width(cont_winch_box, 1, 0);
        lv_obj_set_style_border_color(cont_winch_box, lv_palette_main(LV_PALETTE_GREY), 0);
      }

      lv_obj_set_style_bg_color(btn_winch_in, lv_palette_main(LV_PALETTE_GREY), 0);
      lv_obj_set_style_bg_color(btn_winch_out, lv_palette_main(LV_PALETTE_GREY), 0);
    } else {
      // Sperre aus: Normalen Rahmen wiederherstellen
      winchBlockFlashUntilMs = 0;
      lv_obj_set_style_border_width(cont_winch_box, 1, 0);
      lv_obj_set_style_border_color(cont_winch_box, lv_palette_main(LV_PALETTE_GREY), 0);

      bool active_in = move_in_btn || hw_moving_in;
      bool active_out = move_out_btn || hw_moving_out;

      lv_obj_set_style_bg_color(btn_winch_in,
                                active_in ? lv_palette_main(LV_PALETTE_YELLOW) : lv_palette_main(LV_PALETTE_BLUE),
                                0);
      lv_obj_set_style_bg_color(btn_winch_out,
                                active_out ? lv_palette_main(LV_PALETTE_YELLOW) : lv_palette_main(LV_PALETTE_BLUE),
                                0);
    }
  }

  if (isConnected && incomingTelemetry.liveDataReceived == 1) {
    float pitch_val = incomingTelemetry.pitch;
    float roll_val = incomingTelemetry.roll;

    float pitch_limit = 30.0f;
    float roll_limit = 30.0f;

    if (cfg && !pairedConfigEmpty) {
      for (int i = 0; i < ch_count; i++) {
        if (strcmp(cfg->channels[i].name, "PITCH") == 0) {
          if (cfg->channels[i].constrainMax > 0) pitch_limit = (float)cfg->channels[i].constrainMax;
        } else if (strcmp(cfg->channels[i].name, "ROLL") == 0) {
          if (cfg->channels[i].constrainMax > 0) roll_limit = (float)cfg->channels[i].constrainMax;
        }
      }
    }

    // 1. Getrennte Pitch-Box aktualisieren
    if (lv_obj_is_valid(lbl_pitch_val)) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Nick: %+.1f deg", pitch_val);
      lv_label_set_text(lbl_pitch_val, buf);

      float ratio = abs(pitch_val) / pitch_limit;
      lv_obj_t *parent_box = lv_obj_get_parent(lbl_pitch_val);
      if (ratio >= 1.0f) {
        lv_obj_set_style_bg_color(parent_box, lv_color_hex(0xFFC0C0), 0);
      } else if (ratio >= 0.8f) {
        lv_obj_set_style_bg_color(parent_box, lv_color_hex(0xFFFACD), 0);
      } else {
        lv_obj_set_style_bg_color(parent_box, lv_color_hex(0xD0F0D0), 0);
      }
    }

    // 2. Getrennte Roll-Box aktualisieren
    if (lv_obj_is_valid(lbl_roll_val)) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Roll: %+.1f deg", roll_val);
      lv_label_set_text(lbl_roll_val, buf);

      float ratio = abs(roll_val) / roll_limit;
      lv_obj_t *parent_box = lv_obj_get_parent(lbl_roll_val);
      if (ratio >= 1.0f) {
        lv_obj_set_style_bg_color(parent_box, lv_color_hex(0xFFC0C0), 0);
      } else if (ratio >= 0.8f) {
        lv_obj_set_style_bg_color(parent_box, lv_color_hex(0xFFFACD), 0);
      } else {
        lv_obj_set_style_bg_color(parent_box, lv_color_hex(0xD0F0D0), 0);
      }
    }

    // 3. Gang-Leiste aktualisieren
    if (incomingTelemetry.currentGear != currentActiveGear) {
      currentActiveGear = incomingTelemetry.currentGear;
      rebuild_dynamic_gear_bar();
    }
  } else {
    if (lv_obj_is_valid(lbl_pitch_val)) lv_label_set_text(lbl_pitch_val, "Nick: --- deg");
    if (lv_obj_is_valid(lbl_roll_val)) lv_label_set_text(lbl_roll_val, "Roll: --- deg");
  }
}

/**
 * @brief Erneuert die dynamische Gang-Schaltleiste aus der Kanaldefinition (GEARBOX states) des aktiven Modells.
 */
void rebuild_dynamic_gear_bar(void) {
  if (!cont_gear_bar || !lv_obj_is_valid(cont_gear_bar)) return;
  lv_obj_clean(cont_gear_bar);

  bool pairedActive = has_active_paired_model();
  const ModelConfig *cfg = get_active_dashboard_config();
  int ch_count = get_safe_channel_count(cfg);
  const char* states_str = NULL;

  if (pairedActive && ch_count <= 0) {
    lv_obj_t *lbl_warn = lv_label_create(cont_gear_bar);
    lv_label_set_text(lbl_warn, "KEINE CONFIG GELADEN");
    lv_obj_set_style_text_font(lbl_warn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_warn, lv_palette_main(LV_PALETTE_RED), 0);
    return;
  }

  if (cfg) {
    for (int i = 0; i < ch_count; i++) {
      if (cfg->channels[i].type == TYPE_MULTI_STATE || strcmp(cfg->channels[i].name, "GEARBOX") == 0) {
        if (strlen(cfg->channels[i].states) > 0) {
          states_str = cfg->channels[i].states;
          break;
        }
      }
    }
  }

  if (states_str == NULL || strlen(states_str) == 0) {
    states_str = "R2,R1,N,1,2,3,4,5,6";
  }

  char temp_states[64];
  strncpy(temp_states, states_str, sizeof(temp_states) - 1);
  temp_states[sizeof(temp_states) - 1] = '\0';

  char* token = strtok(temp_states, ",");
  int index = 0;

  while (token != NULL) {
    while (*token == ' ') token++;

    lv_obj_t *gear_box = lv_obj_create(cont_gear_bar);
    lv_obj_set_size(gear_box, 36, 26);
    lv_obj_set_style_radius(gear_box, 0, 0);
    lv_obj_set_style_pad_all(gear_box, 0, 0);
    lv_obj_set_scrollable(gear_box, false);

    if (index == currentActiveGear) {
      lv_obj_set_style_border_width(gear_box, 2, 0);
      lv_obj_set_style_border_color(gear_box, lv_palette_main(LV_PALETTE_RED), 0);
      lv_obj_set_style_bg_color(gear_box, lv_color_hex(0x300000), 0);
    } else {
      lv_obj_set_style_border_width(gear_box, 1, 0);
      lv_obj_set_style_border_color(gear_box, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
    }

    lv_obj_t *lbl_gear = lv_label_create(gear_box);
    lv_label_set_text(lbl_gear, token);
    lv_obj_center(lbl_gear);
    lv_obj_set_style_text_font(lbl_gear, &lv_font_montserrat_12, 0);

    token = strtok(NULL, ",");
    index++;
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Gang-Leiste (v1.3.1) aufgebaut mit Zustaenden: %s\n", states_str);
  }
}

/**
 * @brief Raeumt den Speicher des Dashboard-Tabs beim Verlassen vollstaendig ab.
 */
void cleanup_dashboard_tab(void) {
  if (popup_vol_win != NULL && lv_obj_is_valid(popup_vol_win)) {
    lv_obj_delete(popup_vol_win);
  }
  popup_vol_win = NULL;
  lbl_popup_vol_level = NULL;

  if (tab_dashboard && lv_obj_is_valid(tab_dashboard)) {
    lv_obj_clean(tab_dashboard);
  }
  cont_gear_bar = NULL;
  btn_gear_mode = NULL;
  lbl_gear_mode = NULL;
  cont_lighting_col = NULL;
  cont_right_col = NULL;
  lbl_pitch_val = NULL;
  lbl_roll_val = NULL;
  cont_winch_box = NULL;
  btn_winch_in = NULL;
  btn_winch_out = NULL;
  lbl_missing_cfg_status = NULL;
  btn_audio_block = NULL;
  lbl_volume_val = NULL;

  lastConnectedState = false;
  lastObservedModelIdx = -127;
  lastObservedPairedChCount = -1;

  portENTER_CRITICAL(&winchMux);
  winchMovingIn = false;
  winchMovingOut = false;
  portEXIT_CRITICAL(&winchMux);

  winchBlockFlashUntilMs = 0;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    light_widgets[i] = NULL;
    light_is_hw_driven[i] = false;
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[DEBUG] Dashboard-Tab Speicher vollstaendig bereinigt.");
  }
  print_vram_status("Dashboard-Tab Cleanup");
}

#pragma endregion