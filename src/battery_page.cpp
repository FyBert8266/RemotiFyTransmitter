/**
 * @file battery_page.cpp
 * @version 1.0.2
 * @brief Implementierung des Akku-Tabs mit Live-Metriken, Gitternetzlinien und permanenter Hintergrund-Historienaufzeichnung (v1.0.2).
 */


#include <lvgl.h>
#include <Arduino.h>
#include <config_types.h>
#include "SerialInterface.h"
#include "battery_page.h"
#include "hardware_io.h"
#include "espnow_comm.h"

// ============================================================
// GLOBALE VARIABLEN & UI-REFERENZEN
// ============================================================

static lv_obj_t *tab_battery = NULL;
static lv_obj_t *chart_battery = NULL;
static lv_chart_series_t *ser_sender_pct = NULL;
static lv_chart_series_t *ser_model_pct = NULL;

static lv_obj_t *lbl_sender_header = NULL;
static lv_obj_t *lbl_model_header = NULL;
static lv_obj_t *lbl_footer_info = NULL;

// Persistente Historien-Puffer fuer das Diagramm, um Datenverlust beim Tabwechsel zu verhindern
#define MAX_CHART_POINTS 120
static int16_t sender_history[MAX_CHART_POINTS];
static int16_t model_history[MAX_CHART_POINTS];
static int chart_data_count = 0;

// ============================================================
// DEBUG & VRAM HILFSFUNKTIONEN
// ============================================================

/**
 * @brief Gibt den aktuellen VRAM-Speicherstatus von LVGL aus.
 * @param tag Bezeichner fuer die Debug-Ausgabe.
 */
static void print_vram_status(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Speicherauslastung: %d%% | Gesamt: %d Bytes | Frei: %d Bytes | Groesster Block: %d Bytes\n",
                tag,
                (int)mon.used_pct, 
                (int)mon.total_size, 
                (int)mon.free_size,
                (int)mon.frag_pct);
  }
}

// ============================================================
// ERSTELLUNG UND BEREINIGUNG DES AKKU-TABS
// ============================================================

/**
 * @brief Erstellt und initialisiert den Akku-Tab im uebergebenen Tab-Container.
 * @param parent_tab Zeiger auf den Ziel-Tab der Tabview.
 */
void build_battery_tab(lv_obj_t *parent_tab) {
  tab_battery = parent_tab;

  // Vertikales Flexbox-Layout ohne Scrollen
  lv_obj_set_flex_flow(tab_battery, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tab_battery, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(tab_battery, 6, 0);
  lv_obj_set_style_pad_row(tab_battery, 4, 0);
  lv_obj_set_scrollable(tab_battery, false);

  // Kopfzeile: Zwei Spannungsanzeigen
  lv_obj_t *header_cont = lv_obj_create(tab_battery);
  lv_obj_set_size(header_cont, 450, 36);
  lv_obj_set_flex_flow(header_cont, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(header_cont, 4, 0);
  lv_obj_set_style_border_width(header_cont, 0, 0);
  lv_obj_set_style_bg_opa(header_cont, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollable(header_cont, false);

  lbl_sender_header = lv_label_create(header_cont);
  lv_label_set_text(lbl_sender_header, "Sender: --.---V (--.-%)");
  lv_obj_set_style_text_font(lbl_sender_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_sender_header, lv_palette_main(LV_PALETTE_BLUE), 0);

  lbl_model_header = lv_label_create(header_cont);
  lv_label_set_text(lbl_model_header, "Fahrakku: --.---V (--.-%)");
  lv_obj_set_style_text_font(lbl_model_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_model_header, lv_palette_main(LV_PALETTE_GREEN), 0);

  // Diagramm
  chart_battery = lv_chart_create(tab_battery);
  lv_obj_set_size(chart_battery, 450, 195);
  lv_chart_set_type(chart_battery, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart_battery, MAX_CHART_POINTS);
  
  lv_chart_set_range(chart_battery, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_range(chart_battery, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
  
  // Gitternetzlinien fuer LVGL 9 setzen (4 horizontale, 6 vertikale Linien)
  lv_chart_set_div_line_count(chart_battery, 4, 6);

  lv_obj_set_style_line_width(chart_battery, 2, LV_PART_ITEMS);
  lv_obj_set_style_opa(chart_battery, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_size(chart_battery, 0, 0, LV_PART_INDICATOR);

  ser_sender_pct = lv_chart_add_series(chart_battery, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_SECONDARY_Y);
  ser_model_pct  = lv_chart_add_series(chart_battery, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_SECONDARY_Y);

  // Diagramm mit den gespeicherten Historien-Daten befuellen (Verhindert Datenverlust beim Tabwechsel)
  for (int i = 0; i < chart_data_count; i++) {
    lv_chart_set_value_by_id(chart_battery, ser_sender_pct, i, sender_history[i]);
    lv_chart_set_value_by_id(chart_battery, ser_model_pct, i, model_history[i]);
  }
  lv_chart_refresh(chart_battery);

  // Fusszeile
  lbl_footer_info = lv_label_create(tab_battery);
  lv_label_set_text(lbl_footer_info, "Send: Betr. 00:00:00 (Rest: -- min) | Auto: Rest: -- min");
  lv_obj_set_style_text_font(lbl_footer_info, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_align(lbl_footer_info, LV_TEXT_ALIGN_CENTER, 0);

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Battery)) {
  SerialInterface::printDebug(SerialInterface::Module::Battery, "[DEBUG] Akku-Tab erfolgreich aufgebaut und Historie geladen.");
  }
  print_vram_status("Akku-Tab Aufbau");
}

/**
 * @brief Raeumt den Speicher des Akku-Tabs beim Verlassen vollstaendig ab.
 */
void cleanup_battery_tab(void) {
  if (tab_battery && lv_obj_is_valid(tab_battery)) {
    lv_obj_clean(tab_battery);
  }
  chart_battery = NULL;
  ser_sender_pct = NULL;
  ser_model_pct = NULL;
  lbl_sender_header = NULL;
  lbl_model_header = NULL;
  lbl_footer_info = NULL;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Battery)) {
  SerialInterface::printDebug(SerialInterface::Module::Battery, "[DEBUG] Akku-Tab Speicher vollstaendig bereinigt.");
  }
  print_vram_status("Akku-Tab Cleanup");
}

// ============================================================
// PERIODISCHE AKTUALISIERUNG UND HINTERGRUND-HISTORIE
// ============================================================

/**
 * @brief Aktualisiert periodisch die Akku-Werte, zeichnet die Historie im Hintergrund auf und aktualisiert die UI, falls aktiv.
 */
void update_battery_ui(void) {
  float sender_voltage = get_battery_voltage();
  float sender_percentage = get_battery_percentage();
  uint32_t sender_uptime = get_uptime_seconds();
  
  bool charging = is_battery_charging();
  float charge_rate = maxlipo.chargeRate();
  if (isnan(charge_rate)) charge_rate = 0.0f;

  uint32_t sender_time_sec = charging ? get_estimated_charging_seconds() : get_estimated_remaining_seconds();
  uint32_t sender_time_mins = sender_time_sec / 60;

  bool modelConnected = (hasTelemetry && (millis() - lastTelemetryTime < 2000));
  int model_rem_mins = 0;
  if (modelConnected && incomingTelemetry.liveDataReceived) {
    model_rem_mins = (int)incomingTelemetry.remainingMins;
  }

  // 1. Hintergrund-Historienaufzeichnung (erfolgt IMMER, unabhaengig davon, ob der Tab aktiv ist)
  static unsigned long lastChartUpdateMs = 0;
  unsigned long currentMillis = millis();

  if (lastChartUpdateMs == 0 || (currentMillis - lastChartUpdateMs >= 60000)) {
    lastChartUpdateMs = currentMillis;

    int16_t sender_pct_val = (int16_t)constrain(sender_percentage, 0.0f, 100.0f);
    int16_t model_pct_val = 0;
    if (modelConnected && incomingTelemetry.liveDataReceived) {
      model_pct_val = (int16_t)constrain(incomingTelemetry.percent, 0.0f, 100.0f);
    }

    // Puffer aktualisieren (Verschieben bei Kapazitaetsgrenze)
    if (chart_data_count < MAX_CHART_POINTS) {
      sender_history[chart_data_count] = sender_pct_val;
      model_history[chart_data_count] = model_pct_val;
      chart_data_count++;
    } else {
      memmove(&sender_history[0], &sender_history[1], (MAX_CHART_POINTS - 1) * sizeof(int16_t));
      memmove(&model_history[0], &model_history[1], (MAX_CHART_POINTS - 1) * sizeof(int16_t));
      sender_history[MAX_CHART_POINTS - 1] = sender_pct_val;
      model_history[MAX_CHART_POINTS - 1] = model_pct_val;
    }

    // Live im Diagramm aktualisieren, falls das Widget gueltig ist (Tab ist aktiv)
    if (chart_battery && lv_obj_is_valid(chart_battery)) {
      lv_chart_set_next_value(chart_battery, ser_sender_pct, sender_pct_val);
      lv_chart_set_next_value(chart_battery, ser_model_pct, model_pct_val);
      lv_chart_refresh(chart_battery);
    }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Battery)) {
    SerialInterface::printDebug(SerialInterface::Module::Battery, "[DEBUG] Akku-Historie im Hintergrund aktualisiert -> Sender: %d%%, Modell: %d%%\n", 
                  sender_pct_val, model_pct_val);
  }
  }

  // 2. UI-Aktualisierung (nur wenn der Akku-Tab aktiv und gueltig ist)
  if (!tab_battery || !lv_obj_is_valid(tab_battery)) return;

  int sv_int = (int)sender_voltage;
  int sv_dec = abs((int)((sender_voltage - sv_int) * 1000));
  int sp_int = (int)sender_percentage;
  int sp_dec = abs((int)((sender_percentage - sp_int) * 10));

  int cr_int = (int)charge_rate;
  int cr_dec = abs((int)((charge_rate - cr_int) * 10));

  if (charging && charge_rate > 0.0f) {
    lv_label_set_text_fmt(lbl_sender_header, "Sender: %d.%03dV (%d.%d%% +%d.%d%%/h)", 
                          sv_int, sv_dec, sp_int, sp_dec, cr_int, cr_dec);
  } else {
    lv_label_set_text_fmt(lbl_sender_header, "Sender: %d.%03dV (%d.%d%% %d.%d%%/h)", 
                          sv_int, sv_dec, sp_int, sp_dec, cr_int, cr_dec);
  }

  if (modelConnected && incomingTelemetry.liveDataReceived) {
    int mv_int = (int)incomingTelemetry.voltage;
    int mv_dec = abs((int)((incomingTelemetry.voltage - mv_int) * 1000));
    int mp_int = (int)incomingTelemetry.percent;
    int mp_dec = abs((int)((incomingTelemetry.percent - mp_int) * 10));

    lv_label_set_text_fmt(lbl_model_header, "Fahrakku: %d.%03dV (%d.%d%%)", mv_int, mv_dec, mp_int, mp_dec);
  } else {
    lv_label_set_text_fmt(lbl_model_header, "Fahrakku: --.---V (--.-%)");
  }

  uint32_t up_h = sender_uptime / 3600;
  uint32_t up_m = (sender_uptime % 3600) / 60;
  uint32_t up_s = sender_uptime % 60;

  if (modelConnected && incomingTelemetry.liveDataReceived) {
    if (charging) {
      lv_label_set_text_fmt(lbl_footer_info, "Send: Betr. %02lu:%02lu:%02lu (Voll in: %lu min) | Auto: Rest: %d min",
                            up_h, up_m, up_s, (unsigned long)sender_time_mins, model_rem_mins);
    } else {
      lv_label_set_text_fmt(lbl_footer_info, "Send: Betr. %02lu:%02lu:%02lu (Rest: %lu min) | Auto: Rest: %d min",
                            up_h, up_m, up_s, (unsigned long)sender_time_mins, model_rem_mins);
    }
  } else {
    if (charging) {
      lv_label_set_text_fmt(lbl_footer_info, "Send: Betr. %02lu:%02lu:%02lu (Voll in: %lu min) | Auto: Rest: -- min",
                            up_h, up_m, up_s, (unsigned long)sender_time_mins);
    } else {
      lv_label_set_text_fmt(lbl_footer_info, "Send: Betr. %02lu:%02lu:%02lu (Rest: -- min) | Auto: Rest: -- min",
                            up_h, up_m, up_s, (unsigned long)sender_time_mins);
    }
  }
}