#ifndef MODELS_PAGE_H
#define MODELS_PAGE_H

/**
 * @file models_page.h
 * @version 1.1.0
 * @brief Header-Datei fuer den Haupt-Tab "Modelle" (Multi-Model-Verwaltung, Pairing-Modus,
 *        Sortierung, Speichern-Button je Modellzeile und Loeschen).
 * @Version_info v1.1.0: refresh_models_list_ui() und model_save_row_btn_cb() Deklaration ergaenzt.
 *               v1.0.0: Initiale Erstellung der Modells-Verwaltungsseite.
 */

#ifndef LV_CONF_H
#include <lvgl.h>
#endif

#pragma region FUNCTION_PROTOTYPES
/**
 * @brief Erstellt und baut den Haupt-Tab "Modelle" in der Tabview auf.
 * @param parent_tab Das LVGL-Objekt des Eltern-Tabs.
 */
void build_models_page(lv_obj_t *parent_tab);

/**
 * @brief Aktualisiert periodisch die Inhalte und den Status der Modell-Verwaltungsseite.
 */
void update_models_page_ui(void);

/**
 * @brief Aktualisiert die Liste der gepairten Modelle auf der UI strikt sortiert nach sortOrder.
 */
void refresh_models_list_ui(void);

/**
 * @brief Raeumt alle Ressourcen der Modell-Seite beim Seitenwechsel ab.
 */
void cleanup_models_page(void);
#pragma endregion

#endif // MODELS_PAGE_H