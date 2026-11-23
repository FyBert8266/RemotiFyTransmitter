/**
 * @file boot_splash.cpp
 * @version 1.0.1
 * @brief Implementierung des grafischen Boot-Splash-Screens mit Fortschrittsbalken (v1.0.1).
 * @Version_info v1.0.1: Umstellung von LV_ANIM_ON auf LV_ANIM_OFF und Ergaenzung von lv_tick_inc zur Erzwingung des LVGL-Redraws im setup().
 */

#include "boot_splash.h"
#include "SerialInterface.h"

static lv_obj_t *splash_screen = NULL;
static lv_obj_t *lbl_boot_status = NULL;
static lv_obj_t *bar_boot_progress = NULL;

void show_boot_splash_screen(void) {
  splash_screen = lv_obj_create(lv_screen_active());
  lv_obj_set_size(splash_screen, 480, 320);
  lv_obj_center(splash_screen);
  lv_obj_set_style_bg_color(splash_screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(splash_screen, 0, 0);
  lv_obj_set_style_pad_all(splash_screen, 0, 0);
  lv_obj_set_style_radius(splash_screen, 0, 0);
  lv_obj_set_scrollable(splash_screen, false);

  // Titel der Anwendung
  lv_obj_t *lbl_title = lv_label_create(splash_screen);
  lv_label_set_text(lbl_title, "RemotiFy RC-Sender");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lbl_title, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, -50);

  // Fortschrittsbalken
  bar_boot_progress = lv_bar_create(splash_screen);
  lv_obj_set_size(bar_boot_progress, 340, 20);
  lv_obj_align(bar_boot_progress, LV_ALIGN_CENTER, 0, 10);
  lv_bar_set_range(bar_boot_progress, 0, 100);
  lv_bar_set_value(bar_boot_progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_boot_progress, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_boot_progress, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);

  // Status-Text Label
  lbl_boot_status = lv_label_create(splash_screen);
  lv_label_set_text(lbl_boot_status, "System wird gestartet...");
  lv_obj_set_style_text_font(lbl_boot_status, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_boot_status, lv_color_hex(0xAAAAAA), 0);
  lv_obj_align(lbl_boot_status, LV_ALIGN_CENTER, 0, 50);

  lv_tick_inc(50);
  lv_timer_handler();
  SerialInterface::printSystem("[BOOT] Splash Screen erfolgreich angezeigt.");
}

void update_boot_progress(uint8_t progress, const char *status_text) {
  if (bar_boot_progress && lv_obj_is_valid(bar_boot_progress)) {
    // Wichtig: LV_ANIM_OFF verwenden, damit der Wert sofort hart gesetzt wird
    lv_bar_set_value(bar_boot_progress, progress, LV_ANIM_OFF);
  }
  if (lbl_boot_status && lv_obj_is_valid(lbl_boot_status)) {
    lv_label_set_text(lbl_boot_status, status_text);
  }
  
  // Zeitgeber fuer LVGL v9 vorschieben und Redraw erzwingen
  lv_tick_inc(50);
  lv_timer_handler();
  delay(250); 

  SerialInterface::printSystem("[BOOT] [%3d%%] %s", progress, status_text);
}

void hide_boot_splash_screen(void) {
  if (splash_screen && lv_obj_is_valid(splash_screen)) {
    lv_obj_delete(splash_screen);
    splash_screen = NULL;
  }
  SerialInterface::printSystem("[BOOT] Splash Screen ausgeblendet. Haupt-UI aktiv.");
}