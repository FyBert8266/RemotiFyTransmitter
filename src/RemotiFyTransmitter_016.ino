/**
 * @file RemotiFyTransmitter.ino
 * @version 1.1.3
 * @brief Haupt-Sketch fuer den RC-Sender auf Basis ESP32 und LVGL 9 mit korrigierten Funktionsaufrufen (v1.1.3).
 * @Version_info v012: Korrekte Funktion "Rcv senden"-Button auf Model_page hergestellt.
 * @Version_info v013: Korrekte Funktion von Zustandsanzeige Beleuchtungskanal und Momentary-Button Beleuchtungskanal hergestellt. Funktion Winden-Block korrekt hergestellt.
 * @Version_info v014: ...
 * @Version_info v013: ...
 * @Version_info v013: ...
 * @Version_info v013: ...
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "LittleFS.h"

#include "config_types.h"
#include "hardware_io.h"
#include "dashboard_page.h"
#include "espnow_comm.h"
#include "models_page.h"
#include "model_page.h"
#include "system_page.h"
#include "battery_page.h"
#include "channel_subpage.h"
#include "channel_processor.h"
#include "SerialInterface.h"
#include "boot_splash.h"
#include "core_tasks.h"
#include "audio_module.h"
#include "pairing_manager.h"

// ============================================================
// KONSTANTEN & GLOBALE VARIABLEN
// ============================================================

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 320

TFT_eSPI tft = TFT_eSPI();

// Globale Variablen fuer Datenhandling
ModelConfig currentConfig;
ModelConfig lastSavedConfig;
bool is_config_changed = false;

lv_obj_t *tabview = NULL;
lv_obj_t *tab_dashboard = NULL;
lv_obj_t *tab_battery = NULL;
lv_obj_t *tab_models = NULL;
lv_obj_t *tab_configs = NULL;
lv_obj_t *tab_system = NULL;

lv_obj_t *status_bar = NULL;

static int32_t last_active_idx = -1;
lv_chart_series_t *ser_battery = NULL;
unsigned long last_touch_time = 0;

// ============================================================
// DISPLAY & TOUCH CALLBACKS
// ============================================================

/**
 * @brief Display Flush Callback fuer LVGL v9 mit optionaler FPS-Zaehlung.
 */
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();
  lv_display_flush_ready(disp);

}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t touchX = 0, touchY = 0;
  bool touched = tft.getTouch(&touchX, &touchY, 600);

  if (!touched) {
    data->state = LV_INDEV_STATE_RELEASED;
  } else {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = (int32_t)touchX;
    data->point.y = (int32_t)touchY;
    notify_user_touch_activity(); // Sofortiges Aufwecken & Zeitstempel-Reset auf jedem Tab
  }
}

// ============================================================
// SPEICHER- & VRAM-DIAGNOSE
// ============================================================

void print_memory_status(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  uint32_t free_heap = ESP.getFreeHeap();
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[MEM DIAGNOSE] [%s] Freier Heap: %u Bytes\n", tag, free_heap);
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Speicherauslastung: %d%% | Frei: %d Bytes\n",
                tag, (int)mon.used_pct, (int)mon.free_size);
  }
}

// ============================================================
// UI-AUFBAU & LAZY LOADING
// ============================================================

static void tabview_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_VALUE_CHANGED) {
    uint32_t active_idx = lv_tabview_get_tab_active(tv);
    if ((int32_t)active_idx == last_active_idx) return;

    if (last_active_idx >= 0) {
      if (last_active_idx == 0) cleanup_dashboard_tab();
      else if (last_active_idx == 1) cleanup_battery_tab();
      else if (last_active_idx == 2) cleanup_models_page();
      else if (last_active_idx == 3) cleanup_channel_list_page();
      else if (last_active_idx == 4) cleanup_system_page();
    }

    if (active_idx == 0) build_dashboard_tab(tab_dashboard);
    else if (active_idx == 1) build_battery_tab(tab_battery);
    else if (active_idx == 2) build_models_page(tab_models);
    else if (active_idx == 3) build_channel_list_page();
    else if (active_idx == 4) build_system_page(tab_system);

    last_active_idx = (int32_t)active_idx;
  }
}

void build_professional_rc_ui() {
  lv_theme_t *th = lv_theme_default_init(NULL, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_CYAN), true, LV_FONT_DEFAULT);
  lv_display_set_theme(NULL, th);
  
  status_bar = lv_obj_create(lv_screen_active());
  lv_obj_set_size(status_bar, SCREEN_WIDTH, 25);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(status_bar, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_style_pad_all(status_bar, 0, 0);
  lv_obj_set_style_border_width(status_bar, 0, 0);
  
  update_top_bar_ui(status_bar); 
  
  tabview = lv_tabview_create(lv_screen_active());
  lv_obj_set_scrollable(lv_tabview_get_content(tabview), false);
  lv_tabview_set_tab_bar_position(tabview, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(tabview, 35);
  lv_obj_align(tabview, LV_ALIGN_TOP_MID, 0, 25);
  lv_obj_set_size(tabview, SCREEN_WIDTH, SCREEN_HEIGHT - 25);
  lv_obj_set_scrollable(tabview, false);
  
  // Exakte Tab-Reihenfolge: Dashboard, Akku, Modelle, Configs, System
  tab_dashboard = lv_tabview_add_tab(tabview, "Dashboard");
  tab_battery = lv_tabview_add_tab(tabview, "Akku");
  tab_models = lv_tabview_add_tab(tabview, "Modelle");
  tab_configs = lv_tabview_add_tab(tabview, "Configs");
  tab_system = lv_tabview_add_tab(tabview, "System");

  lv_obj_add_event_cb(tabview, tabview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  last_active_idx = 0;
  build_dashboard_tab(tab_dashboard);
}


// ============================================================
// SETUP & MAIN LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  tft.begin();
  tft.setRotation(1);
  uint16_t calData[5] = { 286, 3635, 264, 3541, 7 };
  tft.setTouch(calData);

  lv_init();
  lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_color_t *buf = (lv_color_t *)malloc(SCREEN_WIDTH * 40 * sizeof(lv_color_t));
  if (buf != NULL) {
    lv_display_set_buffers(disp, buf, NULL, SCREEN_WIDTH * 40 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
  }
  lv_display_set_flush_cb(disp, my_disp_flush);
  
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);
  lv_indev_set_display(indev, disp);

  show_boot_splash_screen();
  update_boot_progress(15, "Lade Dateisystem (LittleFS)...");

  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[LITTLEFS][ERROR] Mount der Partition 'littlefs' fehlgeschlagen.");
  }
  SerialInterface::begin();
  
  
  update_boot_progress(35, "Initialisiere Hardware-IO & I2C...");
  init_hardware_io();
  init_audio_module();

  update_boot_progress(45, "Aktiviere Display-Helligkeit...");
  init_backlight();

  update_boot_progress(55, "Starte I2C-Sensoren & Erweiterungen...");
  init_hardware_extensions();

  update_boot_progress(75, "Starte ESP-NOW Funkmodul...");
  init_espnow_comm();
  

  update_boot_progress(90, "Lade letzte Modellkonfiguration...");
  memset(&currentConfig, 0, sizeof(ModelConfig));
  snprintf(currentConfig.modelName, MAX_NAME_LEN, "RemotiFy Buggy");
  currentConfig.channelCount = 2;
  snprintf(currentConfig.channels[0].name, MAX_NAME_LEN, "Lenkung");
  snprintf(currentConfig.channels[1].name, MAX_NAME_LEN, "Gas");
  lastSavedConfig = currentConfig;
  loadLastUsedModelOnBoot();

  update_boot_progress(100, "System bereit. Starte Benutzeroberflaeche...");
  
  hide_boot_splash_screen();
  build_professional_rc_ui();
  update_top_bar_ui(status_bar);
  init_core_tasks();
}

void loop() {
  SerialInterface::update();
  lv_tick_inc(4);
  lv_timer_handler();
  check_config_changes();
  update_pairing_manager(); // WICHTIG: RegelmÃ¤ÃŸige ÃœberprÃ¼fung des Pairing-Managers (fÃ¼r thread-sichere Popups & Timeout)
  process_pending_received_config(); // Entkoppelte Verarbeitung komplett empfangener Config-Chunks ausserhalb onDataReceive()
  update_async_model_config_sender(); // --- NEU: Asynchronen Config-Sender in jedem Loop-Durchlauf bedienen ---

  static unsigned long lastStatusUpdate = 0;
  if (millis() - lastStatusUpdate >= 1000) {
    lastStatusUpdate = millis();
    update_top_bar_ui(status_bar);
  }

  static unsigned long lastUiUpdate = 0;
  if (millis() - lastUiUpdate > 50) {
    lastUiUpdate = millis();

    // Wenn die Modelle-Seite gerade offen ist oder im Hintergrund aktiv, Countdown/Status aktualisieren
    update_models_page_ui();
    update_system_page_ui();
    if (last_active_idx == 0) {
      update_dashboard_ui();
    }
    update_battery_ui();
    update_model_page_ui();

    if (is_config_page_open && lv_obj_is_valid(dd_hardware_input) && lv_obj_is_valid(lbl_hw_status_val)) {
      uint8_t selected_hw_id = lv_dropdown_get_selected(dd_hardware_input);
      if (selected_hw_id == 0) {
        lv_label_set_text(lbl_hw_status_val, "Deaktiviert");
      } else if (selected_hw_id >= 1 && selected_hw_id <= 4) {
        int16_t raw_val = read_hardware_raw(selected_hw_id);
        const char *axis_name = get_hardware_label(selected_hw_id);
        uint8_t db_setting = (active_channel_idx >= 0 && active_channel_idx < MAX_CHANNELS) ? currentConfig.channels[active_channel_idx].deadband : 0;
        int rc_percent = read_hardware_calibrated_percent(selected_hw_id, raw_val, db_setting);
        lv_label_set_text_fmt(lbl_hw_status_val, "%s: %d%%  (Raw: %d)", axis_name, rc_percent, raw_val);
      }
    }
  }
}