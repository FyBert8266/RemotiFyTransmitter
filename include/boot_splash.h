/**
 * @file boot_splash.h
 * @version 1.0.0
 * @brief Header-Datei fuer den grafischen Boot-Splash-Screen mit Fortschrittsbalken (v1.0.0).
 * @Version_info v1.0.0: Ersterstellung des Splash-Screen-Moduls mit LVGL-Fortschrittsbalken und Live-Statusmeldungen.
 */

#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

#include <lvgl.h>
#include <Arduino.h>

/**
 * @brief Initialisiert und zeigt den grafischen Splash-Screen an.
 */
void show_boot_splash_screen(void);

/**
 * @brief Aktualisiert den Fortschrittsbalken und den Statustext waehrend des Boots.
 * @param progress Prozentualer Fortschritt (0 bis 100).
 * @param status_text Klartextmeldung fuer den aktuellen Init-Schritt.
 */
void update_boot_progress(uint8_t progress, const char *status_text);

/**
 * @brief Entfernt den Splash-Screen nach Abschluss des Bootvorgangs vollstaendig vom Heap.
 */
void hide_boot_splash_screen(void);

#endif // BOOT_SPLASH_H