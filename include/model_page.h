#ifndef MODEL_PAGE_H
#define MODEL_PAGE_H

/**
 * @file model_page.h
 * @version 1.1.0
 * @brief Header-Datei fuer die Modell-Verwaltung, Kanalliste, Modellwechsel-Guard
 *        und Dirty-Status der pairedModels[idx].config.
 * @Version_info v1.1.0: Guard fuer Modellwechsel bei ungespeicherten Aenderungen ergaenzt,
 *               Dirty-Status fuer pairedModels[idx].config ("Modelle *") und Hook-Schnittstellen
 *               fuer Modellwechsel und Verbindungserkennung aufgenommen.
 */

#include <lvgl.h>
#include <Arduino.h>
#include "config_types.h"

// Deklarationen von Funktionen der Modellseite
void build_channel_list_page(void);
void saveConfigToFlash(const ModelConfig &config);
bool loadConfigFromFlash(const char *modelName, ModelConfig &config);
void saveLastUsedModelName(const char *modelName);
void loadLastUsedModelOnBoot(void);
void handle_received_config_logic(void);
void check_config_changes(void);
void cleanup_channel_list_page(void);
void update_model_page_ui(void);

// Modellwechsel, Verbindungsstatus und pairedModels[idx].config Dirty-Verwaltung
bool is_any_paired_model_connected(void);
int get_connected_paired_model_index(void);
void on_model_page_active_model_changed(int newModelIndex);
void request_model_switch_with_guard(int newModelIndex);
bool is_paired_model_config_dirty(int idx);
bool has_any_paired_model_config_dirty(void);
void mark_paired_model_config_dirty(int idx, bool dirty);
void save_paired_model_config_by_index(int idx);
void refresh_models_tab_star_indicator(void);

// Externe globale Variablen
extern lv_obj_t *tab_configs;
extern lv_obj_t *channel_list_subpage;
extern lv_obj_t *ta_model_name;
extern bool is_config_page_open;
extern int active_channel_idx;
extern ModelConfig currentConfig;
extern ModelConfig lastSavedConfig;
extern bool is_config_changed;
extern lv_obj_t *channel_config_subpage;

#endif // MODEL_PAGE_H