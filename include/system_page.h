/**
 * @file system_page.h
 * @version 1.3.0
 * @brief Header-Datei fuer den System-Tab mit linker Tabview und den Sub-Tabs Kalibrierung, Audio, Helligkeit, Namen und System-Info (v1.3.0).
 * @Version_info v1.3.0: Vollstaendige Ersetzung der alten calibration_page durch den neuen System-Tab mit linker Sub-Tabview (LV_DIR_LEFT).
 */

#ifndef SYSTEM_PAGE_H
#define SYSTEM_PAGE_H

#include <lvgl.h>

/**
 * @brief Erstellt und initialisiert den System-Tab im uebergebenen Tab-Container.
 * @param parent_tab Zeiger auf den Ziel-Tab der Tabview.
 */
void build_system_page(lv_obj_t *parent_tab);

/**
 * @brief Raeumt den Speicher des System-Tabs beim Verlassen vollstaendig ab.
 */
void cleanup_system_page(void);

/**
 * @brief Aktualisiert dynamische Live-Werte (Achsen, VRAM, Heap, Auto-Dimming) im Hintergrund.
 */
void update_system_page_ui(void);

/**
 * @brief Meldet eine Benutzerberuehrung und hebt ein aktives Auto-Dimming sofort auf (tab-unabhaengig).
 */
void notify_user_touch_activity(void);

#endif // SYSTEM_PAGE_H