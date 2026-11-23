/**
 * @file battery_page.h
 * @version 1.0.2
 * @brief Header-Datei fuer den Akku-Tab mit Diagramm, Live-Metriken und permanenter Hintergrund-Historienaufzeichnung (v1.0.2).
 */

#ifndef BATTERY_PAGE_H
#define BATTERY_PAGE_H

#include <lvgl.h>

/**
 * @brief Erstellt und initialisiert den Akku-Tab im uebergebenen Tab-Container.
 * @param parent_tab Zeiger auf den Ziel-Tab der Tabview.
 */
void build_battery_tab(lv_obj_t *parent_tab);

/**
 * @brief Raeumt den Speicher des Akku-Tabs beim Verlassen vollstaendig ab.
 */
void cleanup_battery_tab(void);

/**
 * @brief Aktualisiert periodisch die Akku-Werte, zeichnet die Historie im Hintergrund auf und aktualisiert die UI.
 */
void update_battery_ui(void);

#endif // BATTERY_PAGE_H