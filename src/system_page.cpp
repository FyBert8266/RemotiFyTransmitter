/**
 * @file system_page.cpp
 * @version 1.3.4
 * @brief Implementierung des System-Tabs mit vollstaendigen Subtabs (Kalibrierung, Audio mit LautstÃ¤rke-Regler, Helligkeit, Namen, System-Info) (v1.3.4).
 * @Version_info v1.3.4: Vollstaendige Implementierung des Audio-Subtabs mit LautstÃ¤rke-Schieberegler und Event-Filter-Schalter gekoppelt an audio_module.
 */


#include <lvgl.h>
#include <Arduino.h>
#include <cstddef>
#include <config_types.h>
#include "system_page.h"
#include "hardware_io.h"
#include "SerialInterface.h"
#include "audio_module.h"

#pragma region STATIC_VARIABLES
// Externe Deklaration des globalen Touch-Zeitstempels aus dem Haupt-Sketch
extern unsigned long last_touch_time;

// Globale UI-Referenzen fuer den System-Tab
static lv_obj_t *tab_system = NULL;
static lv_obj_t *ta_sender_name = NULL;
static lv_obj_t *kb_sender_name = NULL;
static lv_obj_t *dd_cal_axis = NULL;
static lv_obj_t *lbl_cal_raw_val = NULL;
static lv_obj_t *lbl_cal_min_val = NULL;
static lv_obj_t *lbl_cal_center_val = NULL;
static lv_obj_t *lbl_cal_max_val = NULL;
static lv_obj_t *slider_brightness = NULL; 
static lv_obj_t *lbl_brightness_val = NULL; 
static lv_obj_t *sw_auto_dim = NULL;
static lv_obj_t *slider_volume = NULL;
static lv_obj_t *lbl_volume_val = NULL;
static lv_obj_t *sw_audio_filter = NULL;
static lv_obj_t *lbl_info_heap = NULL;
static lv_obj_t *lbl_info_vram = NULL;

static int active_cal_axis_idx = 0;  
static bool is_auto_dimmed = false;  
#pragma endregion

#pragma region PROTOTYPES
static void cal_axis_select_cb(lv_event_t *e);
static void cal_set_min_cb(lv_event_t *e);
static void cal_set_center_cb(lv_event_t *e);
static void cal_set_max_cb(lv_event_t *e);
static void cal_save_cb(lv_event_t *e);
static void cal_reset_cb(lv_event_t *e);
static void brightness_slider_cb(lv_event_t *e);
static void auto_dim_switch_cb(lv_event_t *e);
static void volume_slider_cb(lv_event_t *e);
static void audio_filter_switch_cb(lv_event_t *e);
static void print_vram_status(const char *tag);
static void sender_name_save_cb(lv_event_t *e);
static void sender_name_ta_event_cb(lv_event_t *e);

void build_calib_subtab(lv_obj_t *parent_side_tabview);
void build_audio_subtab(lv_obj_t *parent_side_tabview);
void build_brightness_subtab(lv_obj_t *parent_side_tabview);
void build_channel_names_subtab(lv_obj_t *parent_side_tabview);
void build_system_info_subtab(lv_obj_t *parent_side_tabview);
void build_sender_name_subtab(lv_obj_t *parent_side_tabview);
#pragma endregion

#pragma region INTERNAL_HELPERS
static void print_vram_status(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Frei: %zu Bytes | Fragmentation: %d%%\n", 
                tag, (int)mon.used_pct, mon.free_size, (int)mon.frag_pct);
  }
}
#pragma endregion

#pragma region PAGE_BUILDERS
void build_system_page(lv_obj_t *parent_tab) {
  tab_system = parent_tab;
  lv_obj_set_scrollable(tab_system, false);
  lv_obj_set_style_pad_all(tab_system, 0, 0);
  lv_obj_set_style_border_width(tab_system, 0, 0);

  // Seitliche Tabview ueber den gesamten verfuegbaren System-Tab legen.
  lv_obj_t *tabview_sys = lv_tabview_create(tab_system);
  lv_tabview_set_tab_bar_position(tabview_sys, LV_DIR_LEFT);
  lv_tabview_set_tab_bar_size(tabview_sys, 120);
  lv_obj_set_size(tabview_sys, LV_PCT(100), LV_PCT(100));
  lv_obj_align(tabview_sys, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_pad_all(tabview_sys, 0, 0);
  lv_obj_set_style_border_width(tabview_sys, 0, 0);

  lv_obj_t *tabview_content = lv_tabview_get_content(tabview_sys);
  lv_obj_set_scrollable(tabview_content, false);
  lv_obj_set_style_pad_all(tabview_content, 0, 0);
  lv_obj_set_style_border_width(tabview_content, 0, 0);

  build_calib_subtab(tabview_sys);
  build_audio_subtab(tabview_sys);
  build_brightness_subtab(tabview_sys);
  build_channel_names_subtab(tabview_sys);
  build_sender_name_subtab(tabview_sys);
  build_system_info_subtab(tabview_sys);

  print_vram_status("BUILD_SYSTEM_PAGE");
}

void build_calib_subtab(lv_obj_t *parent_side_tabview) {
  lv_obj_t *tab_cal_analog = lv_tabview_add_tab(parent_side_tabview, "Kalibrierung");
  lv_obj_set_style_pad_all(tab_cal_analog, 4, 0);
  lv_obj_set_style_border_width(tab_cal_analog, 0, 0);
  lv_obj_set_scrollable(tab_cal_analog, false);

  static int32_t analog_col_dsc[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  static int32_t analog_row_dsc[] = { 34, 28, LV_GRID_FR(1), 36, LV_GRID_TEMPLATE_LAST };
  lv_obj_set_grid_dsc_array(tab_cal_analog, analog_col_dsc, analog_row_dsc);

  dd_cal_axis = lv_dropdown_create(tab_cal_analog);
  lv_obj_set_grid_cell(dd_cal_axis, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  lv_dropdown_set_options(dd_cal_axis, "Joystick L X\nJoystick L Y\nJoystick R X\nJoystick R Y");
  lv_dropdown_set_selected(dd_cal_axis, active_cal_axis_idx);
  lv_obj_set_size(dd_cal_axis, 170, 30);
  lv_obj_add_event_cb(dd_cal_axis, cal_axis_select_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lbl_cal_raw_val = lv_label_create(tab_cal_analog);
  lv_obj_set_grid_cell(lbl_cal_raw_val, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);
  lv_label_set_text(lbl_cal_raw_val, "Live: ---");
  lv_obj_set_style_text_color(lbl_cal_raw_val, lv_palette_main(LV_PALETTE_ORANGE), 0);

  lv_obj_t *cont_vals = lv_obj_create(tab_cal_analog);
  lv_obj_set_grid_cell(cont_vals, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
  static int32_t val_col_dsc[] = { LV_PCT(33), LV_PCT(33), LV_PCT(33), LV_GRID_TEMPLATE_LAST };
  static int32_t val_row_dsc[] = { 26, 32, LV_GRID_TEMPLATE_LAST };
  lv_obj_set_grid_dsc_array(cont_vals, val_col_dsc, val_row_dsc);
  lv_obj_set_style_pad_all(cont_vals, 2, 0);
  lv_obj_set_style_border_width(cont_vals, 0, 0);
  lv_obj_set_style_bg_opa(cont_vals, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollable(cont_vals, false);

  const AnalogCalibration &init_cal = sysCalibration.axes[active_cal_axis_idx];

  lbl_cal_min_val = lv_label_create(cont_vals);
  lv_label_set_text_fmt(lbl_cal_min_val, "Min: %d", init_cal.rawMin);
  lv_obj_set_grid_cell(lbl_cal_min_val, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  lbl_cal_center_val = lv_label_create(cont_vals);
  lv_label_set_text_fmt(lbl_cal_center_val, "Mitte: %d", init_cal.rawCenter);
  lv_obj_set_grid_cell(lbl_cal_center_val, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  lbl_cal_max_val = lv_label_create(cont_vals);
  lv_label_set_text_fmt(lbl_cal_max_val, "Max: %d", init_cal.rawMax);
  lv_obj_set_grid_cell(lbl_cal_max_val, LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  lv_obj_t *btn_min = lv_button_create(cont_vals);
  lv_obj_set_size(btn_min, 95, 28);
  lv_obj_add_event_cb(btn_min, cal_set_min_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_bmin = lv_label_create(btn_min);
  lv_label_set_text(lbl_bmin, "Set Min");
  lv_obj_center(lbl_bmin);
  lv_obj_set_grid_cell(btn_min, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  lv_obj_t *btn_center = lv_button_create(cont_vals);
  lv_obj_set_size(btn_center, 95, 28);
  lv_obj_add_event_cb(btn_center, cal_set_center_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_bcent = lv_label_create(btn_center);
  lv_label_set_text(lbl_bcent, "Set Mitte");
  lv_obj_center(lbl_bcent);
  lv_obj_set_grid_cell(btn_center, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  lv_obj_t *btn_max = lv_button_create(cont_vals);
  lv_obj_set_size(btn_max, 95, 28);
  lv_obj_add_event_cb(btn_max, cal_set_max_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_bmax = lv_label_create(btn_max);
  lv_label_set_text(lbl_bmax, "Set Max");
  lv_obj_center(lbl_bmax);
  lv_obj_set_grid_cell(btn_max, LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  lv_obj_t *cont_cal_actions = lv_obj_create(tab_cal_analog);
  lv_obj_set_grid_cell(cont_cal_actions, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
  lv_obj_set_flex_flow(cont_cal_actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_cal_actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont_cal_actions, 0, 0);
  lv_obj_set_style_border_width(cont_cal_actions, 0, 0);
  lv_obj_set_style_bg_opa(cont_cal_actions, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollable(cont_cal_actions, false);

  lv_obj_t *btn_save_cal = lv_button_create(cont_cal_actions);
  lv_obj_set_size(btn_save_cal, 145, 30);
  lv_obj_set_style_bg_color(btn_save_cal, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(btn_save_cal, cal_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_bsave = lv_label_create(btn_save_cal);
  lv_label_set_text(lbl_bsave, LV_SYMBOL_SAVE " Speichern");
  lv_obj_center(lbl_bsave);

  lv_obj_t *btn_reset_cal = lv_button_create(cont_cal_actions);
  lv_obj_set_size(btn_reset_cal, 145, 30);
  lv_obj_set_style_bg_color(btn_reset_cal, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_add_event_cb(btn_reset_cal, cal_reset_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_breset = lv_label_create(btn_reset_cal);
  lv_label_set_text(lbl_breset, LV_SYMBOL_REFRESH " Reset");
  lv_obj_center(lbl_breset);
}

void build_audio_subtab(lv_obj_t *parent_side_tabview) {
  lv_obj_t *tab_audio = lv_tabview_add_tab(parent_side_tabview, "Audio");
  lv_obj_set_flex_flow(tab_audio, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab_audio, 8, 0);
  lv_obj_set_style_pad_row(tab_audio, 10, 0);
  lv_obj_set_scrollable(tab_audio, false);

  lv_obj_t *lbl_title = lv_label_create(tab_audio);
  lv_label_set_text(lbl_title, "AUDIO & BUZZER EINSTELLUNGEN");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_ORANGE), 0);

  // LautstÃ¤rke-Label
  lbl_volume_val = lv_label_create(tab_audio);
  lv_label_set_text(lbl_volume_val, "Lautstaerke: 3 / 5");
  lv_obj_set_style_text_font(lbl_volume_val, &lv_font_montserrat_12, 0);

  // LautstÃ¤rke-Slider (Stufen 1 bis 5)
  slider_volume = lv_slider_create(tab_audio);
  lv_slider_set_range(slider_volume, 1, 5);
  lv_slider_set_value(slider_volume, 3, LV_ANIM_OFF);
  lv_obj_set_size(slider_volume, 280, 22);
  lv_obj_add_event_cb(slider_volume, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Event-Filter Zeile (Container)
  lv_obj_t *row_audio = lv_obj_create(tab_audio);
  lv_obj_set_size(row_audio, 280, 45);
  lv_obj_set_flex_flow(row_audio, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row_audio, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_border_width(row_audio, 0, 0);
  lv_obj_set_style_bg_opa(row_audio, LV_OPA_TRANSP, 0);

  lv_obj_t *lbl_sw = lv_label_create(row_audio);
  lv_label_set_text(lbl_sw, "Audio-Ausgabe Aktiv");
  
  sw_audio_filter = lv_switch_create(row_audio);
  lv_obj_set_size(sw_audio_filter, 45, 24);
  lv_obj_add_state(sw_audio_filter, LV_STATE_CHECKED); // StandardmÃ¤ÃŸig an
  lv_obj_add_event_cb(sw_audio_filter, audio_filter_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void build_brightness_subtab(lv_obj_t *parent_side_tabview) {
  lv_obj_t *tab_bright = lv_tabview_add_tab(parent_side_tabview, "Helligkeit");
  lv_obj_set_flex_flow(tab_bright, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab_bright, 8, 0);
  lv_obj_set_style_pad_row(tab_bright, 10, 0);
  lv_obj_set_scrollable(tab_bright, false);

  lv_obj_t *lbl_title = lv_label_create(tab_bright);
  lv_label_set_text(lbl_title, "DISPLAY-HELLIGKEIT (IO27 PWM)");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_ORANGE), 0);

  lbl_brightness_val = lv_label_create(tab_bright);
  uint8_t loaded_bright = sysCalibration.displayBrightness;
  if (loaded_bright < 10 || loaded_bright > 100) loaded_bright = 80;
  lv_label_set_text_fmt(lbl_brightness_val, "Helligkeit: %d%%", (int)loaded_bright);

  slider_brightness = lv_slider_create(tab_bright);
  lv_slider_set_range(slider_brightness, 10, 100);
  lv_slider_set_value(slider_brightness, loaded_bright, LV_ANIM_OFF);
  lv_obj_set_size(slider_brightness, 300, 22);
  lv_obj_add_event_cb(slider_brightness, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *cont_dim = lv_obj_create(tab_bright);
  lv_obj_set_size(cont_dim, 320, 40);
  lv_obj_set_flex_flow(cont_dim, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_dim, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont_dim, 2, 0);
  lv_obj_set_style_border_width(cont_dim, 0, 0);
  lv_obj_set_style_bg_opa(cont_dim, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollable(cont_dim, false);

  lv_obj_t *lbl_dim_title = lv_label_create(cont_dim);
  lv_label_set_text(lbl_dim_title, "Auto-Dimming (30s -> 15%)");

  sw_auto_dim = lv_switch_create(cont_dim);
  lv_obj_set_size(sw_auto_dim, 45, 24);
  
  if (sysCalibration.autoDimEnabled) {
    lv_obj_add_state(sw_auto_dim, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(sw_auto_dim, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw_auto_dim, auto_dim_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *btn_save_brightness = lv_button_create(tab_bright);
  lv_obj_set_size(btn_save_brightness, 150, 32);
  lv_obj_set_style_bg_color(btn_save_brightness, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(btn_save_brightness, cal_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_save_brightness = lv_label_create(btn_save_brightness);
  lv_label_set_text(lbl_save_brightness, LV_SYMBOL_SAVE " Speichern");
  lv_obj_center(lbl_save_brightness);
}

/**
 * @brief Baut den scrollbaren Subtab "Kanalnamen" mit Erlaeuterungen zu den Kanal-Schluesselwoertern auf.
 * @param parent_side_tabview Uebergeordneter Tabview-Container.
 */
void build_channel_names_subtab(lv_obj_t *parent_side_tabview) {
  lv_obj_t *tab_names = lv_tabview_add_tab(parent_side_tabview, "Kanalnamen");
  lv_obj_set_flex_flow(tab_names, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab_names, 8, 0);
  lv_obj_set_style_pad_row(tab_names, 8, 0);
  lv_obj_set_style_radius(tab_names, 0, 0);
  lv_obj_set_scrollable(tab_names, true);
  lv_obj_set_scrollbar_mode(tab_names, LV_SCROLLBAR_MODE_AUTO);

  lv_obj_t *lbl_hdr = lv_label_create(tab_names);
  lv_label_set_text(lbl_hdr, "KANALNAMEN & SCHLUESSELWOERTER");
  lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_hdr, lv_palette_main(LV_PALETTE_YELLOW), 0);

  lv_obj_t *lbl_keys = lv_label_create(tab_names);
  lv_label_set_text(lbl_keys,
                    "GEARBOX\n"
                    "Kanal fuer die Gangschaltung. Verfuegbare Gaenge im Feld Zustaende kommasepariert angeben, "
                    "zum Beispiel: R,N,1,2,3,4.\n\n"
                    "PITCH\n"
                    "Kanal fuer die Nickwinkel-Anzeige aus der Telemetrie. constrainMax legt den Grenzwert "
                    "fuer die Warnfarben im Dashboard fest.\n\n"
                    "ROLL\n"
                    "Kanal fuer die Rollwinkel-Anzeige aus der Telemetrie. constrainMax legt den Grenzwert "
                    "fuer die Warnfarben im Dashboard fest.\n\n"
                    "WINCH\n"
                    "Kanal fuer die Windensteuerung. Bei aktiver Sperre wird der Wert auf 1500 us gehalten; "
                    "die Dashboard-Tasten geben 50 % Ausschlag.\n\n"
                    "SOUND\n"
                    "Blendet die Lautstaerkesteuerung im Dashboard ein.\n\n"
                    "LIGHT_<Name>\n"
                    "Erzeugt einen Beleuchtungseintrag im Dashboard. Der Teil nach LIGHT_ wird als Name angezeigt, "
                    "zum Beispiel LIGHT_Fernlicht. Ohne Hardware-Eingang kann der Kanal dort geschaltet werden.");
  lv_obj_set_style_text_font(lbl_keys, &lv_font_montserrat_12, 0);
  lv_obj_set_width(lbl_keys, LV_PCT(100));
  lv_label_set_long_mode(lbl_keys, LV_LABEL_LONG_WRAP);
}

void build_sender_name_subtab(lv_obj_t *parent_side_tabview) {
  lv_obj_t *tab_name = lv_tabview_add_tab(parent_side_tabview, "Sender-Name");
  lv_obj_set_flex_flow(tab_name, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab_name, 8, 0);
  lv_obj_set_style_pad_row(tab_name, 10, 0);
  lv_obj_set_scrollable(tab_name, false);

  lv_obj_t *lbl_title = lv_label_create(tab_name);
  lv_label_set_text(lbl_title, "SENDER-NAME FUER KOPPLUNG");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_ORANGE), 0);

  // Textarea fÃ¼r den Sendernamen
  ta_sender_name = lv_textarea_create(tab_name);
  lv_textarea_set_one_line(ta_sender_name, true);
  lv_textarea_set_max_length(ta_sender_name, 15);
  lv_obj_set_size(ta_sender_name, 280, 40);

  lv_obj_add_event_cb(ta_sender_name, sender_name_ta_event_cb, LV_EVENT_FOCUSED, NULL);
  
  // Aktuellen Namen aus System-Config laden (mit Fallback)
  if (strlen(sysCalibration.senderName) > 0) {
    lv_textarea_set_text(ta_sender_name, sysCalibration.senderName);
  } else {
    lv_textarea_set_text(ta_sender_name, "RemotiFySender");
  }

  // Speicher-Button fÃ¼r den Namen
  lv_obj_t *btn_save_name = lv_button_create(tab_name);
  lv_obj_set_size(btn_save_name, 140, 32);
  lv_obj_set_style_bg_color(btn_save_name, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(btn_save_name, sender_name_save_cb, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t *lbl_btn = lv_label_create(btn_save_name);
  lv_label_set_text(lbl_btn, LV_SYMBOL_SAVE " Name sichern");
  lv_obj_center(lbl_btn);
}

void build_system_info_subtab(lv_obj_t *parent_side_tabview) {
  lv_obj_t *tab_info = lv_tabview_add_tab(parent_side_tabview, "System-Info");
  lv_obj_set_flex_flow(tab_info, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab_info, 8, 0);
  lv_obj_set_scrollable(tab_info, false);

  lv_obj_t *lbl_title = lv_label_create(tab_info);
  lv_label_set_text(lbl_title, "SYSTEM METADATEN");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_ORANGE), 0);

  lv_obj_t *lbl_ver = lv_label_create(tab_info);
  lv_label_set_text(lbl_ver, "Firmware: RemotiFy v1.3.4 (ESP32 / LVGL 9)");

  lv_obj_t *lbl_sdk = lv_label_create(tab_info);
  lv_label_set_text_fmt(lbl_sdk, "ESP-IDF Version: %s", ESP.getSdkVersion());

  lbl_info_heap = lv_label_create(tab_info);
  lv_label_set_text(lbl_info_heap, "Freier Heap: berechne...");

  lbl_info_vram = lv_label_create(tab_info);
  lv_label_set_text(lbl_info_vram, "LVGL VRAM: aktiv");
}
#pragma endregion

#pragma region CALLBACKS_AND_ACTIONS

static void sender_name_save_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    const char *txt = lv_textarea_get_text(ta_sender_name);
    if (txt && strlen(txt) > 0) {
      strncpy(sysCalibration.senderName, txt, sizeof(sysCalibration.senderName) - 1);
      sysCalibration.senderName[sizeof(sysCalibration.senderName) - 1] = '\0';
      save_calibration(); // Speichert die gesamte SystemCalibrationConfig inkl. Namen im Flash
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
      SerialInterface::printSystem("[DEBUG] Sendername aktualisiert und gespeichert: '%s'", sysCalibration.senderName);
  }
    }
  }
}

static void sender_name_ta_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_FOCUSED) {
    if (kb_sender_name == NULL) {
      // Tastatur erstellen (im aktiven Screen platziert)
      kb_sender_name = lv_keyboard_create(lv_screen_active());
      lv_obj_set_size(kb_sender_name, LV_HOR_RES, LV_VER_RES / 2);
      lv_keyboard_set_textarea(kb_sender_name, ta);
      
      // Event-Callback zum SchlieÃŸen der Tastatur bei 'OK' oder 'Close'
      lv_obj_add_event_cb(kb_sender_name, [](lv_event_t *event) {
        lv_event_code_t c = lv_event_get_code(event);
        if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
          if (kb_sender_name && lv_obj_is_valid(kb_sender_name)) {
            lv_obj_delete(kb_sender_name);
            kb_sender_name = NULL;
          }
        }
      }, LV_EVENT_ALL, NULL);
    }
  }
}

static void cal_axis_select_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    active_cal_axis_idx = lv_dropdown_get_selected(dd_cal_axis);
    const AnalogCalibration &cal = sysCalibration.axes[active_cal_axis_idx];
    lv_label_set_text_fmt(lbl_cal_min_val, "Min: %d", cal.rawMin);
    lv_label_set_text_fmt(lbl_cal_center_val, "Mitte: %d", cal.rawCenter);
    lv_label_set_text_fmt(lbl_cal_max_val, "Max: %d", cal.rawMax);
  }
}

static void cal_set_min_cb(lv_event_t *e) {
  int16_t current_raw = read_hardware_raw(active_cal_axis_idx + 1);
  sysCalibration.axes[active_cal_axis_idx].rawMin = current_raw;
  lv_label_set_text_fmt(lbl_cal_min_val, "Min: %d", current_raw);
}

static void cal_set_center_cb(lv_event_t *e) {
  int16_t current_raw = read_hardware_raw(active_cal_axis_idx + 1);
  sysCalibration.axes[active_cal_axis_idx].rawCenter = current_raw;
  lv_label_set_text_fmt(lbl_cal_center_val, "Mitte: %d", current_raw);
}

static void cal_set_max_cb(lv_event_t *e) {
  int16_t current_raw = read_hardware_raw(active_cal_axis_idx + 1);
  sysCalibration.axes[active_cal_axis_idx].rawMax = current_raw;
  lv_label_set_text_fmt(lbl_cal_max_val, "Max: %d", current_raw);
}

static void cal_save_cb(lv_event_t *e) {
  save_calibration();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
  SerialInterface::printDebug(SerialInterface::Module::System, "[DEBUG] Kalibrierung und System-Einstellungen erfolgreich gespeichert.");
  }
}

static void cal_reset_cb(lv_event_t *e) {
  reset_calibration_to_default();
  const AnalogCalibration &cal = sysCalibration.axes[active_cal_axis_idx];
  lv_label_set_text_fmt(lbl_cal_min_val, "Min: %d", cal.rawMin);
  lv_label_set_text_fmt(lbl_cal_center_val, "Mitte: %d", cal.rawCenter);
  lv_label_set_text_fmt(lbl_cal_max_val, "Max: %d", cal.rawMax);
  
  if (slider_brightness && lv_obj_is_valid(slider_brightness)) {
    lv_slider_set_value(slider_brightness, 80, LV_ANIM_OFF);
    lv_label_set_text(lbl_brightness_val, "Helligkeit: 80%");
  }
  if (sw_auto_dim && lv_obj_is_valid(sw_auto_dim)) {
    lv_obj_clear_state(sw_auto_dim, LV_STATE_CHECKED);
  }
  is_auto_dimmed = false;
  set_display_brightness(80);
}

static void brightness_slider_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    int32_t percent = lv_slider_get_value(slider_brightness);
    if (!is_auto_dimmed) {
      lv_label_set_text_fmt(lbl_brightness_val, "Helligkeit: %d%%", (int)percent);
      set_display_brightness((uint8_t)percent);
    }
  }
}

static void auto_dim_switch_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    bool active = lv_obj_has_state(sw_auto_dim, LV_STATE_CHECKED);
    sysCalibration.autoDimEnabled = active ? 1 : 0;
    
    if (active) {
      last_touch_time = millis(); 
    } else if (is_auto_dimmed && slider_brightness) {
      int32_t val = lv_slider_get_value(slider_brightness);
      set_display_brightness((uint8_t)val);
      is_auto_dimmed = false;
    }
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
    SerialInterface::printDebug(SerialInterface::Module::System, "[DEBUG] Auto-Dimming Schalter: %s\n", active ? "EIN" : "AUS");
  }
  }
}

static void volume_slider_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    int32_t vol = lv_slider_get_value(slider_volume);
    if (lbl_volume_val && lv_obj_is_valid(lbl_volume_val)) {
      lv_label_set_text_fmt(lbl_volume_val, "Lautstaerke: %d / 5", (int)vol);
    }
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
    SerialInterface::printDebug(SerialInterface::Module::System, "[DEBUG] Lautstaerke Stufe geandert auf: %d\n", (int)vol);
  }
  }
}

static void audio_filter_switch_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    bool active = lv_obj_has_state(sw_audio_filter, LV_STATE_CHECKED);
    set_audio_enabled(active);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
    SerialInterface::printDebug(SerialInterface::Module::System, "[DEBUG] Audio-Ausgabe Schalter: %s\n", active ? "EIN" : "AUS");
  }
  }
}

void cleanup_system_page(void) {
  if (kb_sender_name && lv_obj_is_valid(kb_sender_name)) {
    lv_obj_delete(kb_sender_name);
    kb_sender_name = NULL;
  }
  if (tab_system && lv_obj_is_valid(tab_system)) {
    lv_obj_clean(tab_system);
  }
  tab_system = NULL;
  dd_cal_axis = NULL;
  lbl_cal_raw_val = NULL;
  lbl_cal_min_val = NULL;
  lbl_cal_center_val = NULL;
  lbl_cal_max_val = NULL;
  slider_brightness = NULL;
  lbl_brightness_val = NULL;
  sw_auto_dim = NULL;
  slider_volume = NULL;
  lbl_volume_val = NULL;
  sw_audio_filter = NULL;
  lbl_info_heap = NULL;
  lbl_info_vram = NULL;
  is_auto_dimmed = false;
}

void notify_user_touch_activity(void) {
  last_touch_time = millis();

  if (is_auto_dimmed) {
    uint8_t target_val = (slider_brightness && lv_obj_is_valid(slider_brightness))
                           ? (uint8_t)lv_slider_get_value(slider_brightness)
                           : sysCalibration.displayBrightness;
    if (target_val < 15) target_val = 80;
    set_display_brightness(target_val);
    is_auto_dimmed = false;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
    SerialInterface::printSystem("[DEBUG] Auto-Dimming aufgehoben durch Beruehrung -> %d%%.", target_val);
  }
  }
}

void update_system_page_ui(void) {
  if (lbl_cal_raw_val && lv_obj_is_valid(lbl_cal_raw_val)) {
    int16_t raw_val = read_hardware_raw(active_cal_axis_idx + 1);
    lv_label_set_text_fmt(lbl_cal_raw_val, "Live: %d", raw_val);
  }

  if (lbl_info_heap && lv_obj_is_valid(lbl_info_heap)) {
    uint32_t free_heap = ESP.getFreeHeap();
    lv_label_set_text_fmt(lbl_info_heap, "Freier Heap: %u Bytes", (unsigned int)free_heap);
  }

  if (lbl_info_vram && lv_obj_is_valid(lbl_info_vram)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    lv_label_set_text_fmt(lbl_info_vram, "LVGL VRAM Auslastung: %d%% (Frei: %u Bytes)", (int)mon.used_pct, (unsigned int)mon.free_size);
  } else {
    lv_label_set_text(lbl_info_vram, "LVGL VRAM Diagnostik inaktiv");
  }
  }

  if (sysCalibration.autoDimEnabled && !is_auto_dimmed) {
    unsigned long inactive_duration = millis() - last_touch_time;
    if (inactive_duration > 0 && inactive_duration > 30000) {
      set_display_brightness(15);
      is_auto_dimmed = true;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::System)) {
      SerialInterface::printSystem("[DEBUG] Auto-Dimming aktiv: Display auf 15%% gedimmt.");
  }
    }
  }
}
#pragma endregion