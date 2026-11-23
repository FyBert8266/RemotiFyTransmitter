/**
 * @file channel_subpage.h
 * @version 1.0.3
 * @brief Header-Datei fuer die Detail-Konfiguration einzelner Kanaele mit Multi-State Textfeld und Heap-/VRAM-Diagnose (v1.0.3).
 * @Version_info v1.0.3: Ergaenzung des Multi-State Textfelds in Kopfdaten und Korrektur der Failsafe-Grid-Koordinaten.
 */

#ifndef CHANNEL_SUBPAGE_H
#define CHANNEL_SUBPAGE_H

#include <lvgl.h>
#include <Arduino.h>
#include "config_types.h"

// Hauptfunktion zum Oeffnen der Detailseite eines Kanals
void open_channel_config_page(int channel_idx);

// Globale Steuerungsvariablen (extern deklariert)
extern int active_channel_idx;
extern bool is_config_page_open;

// Globale UI-Referenzen der Kanal-Konfigurationsseite
extern lv_obj_t *channel_config_subpage;
extern lv_obj_t *ta_chan_name;
extern lv_obj_t *dd_chan_type;
extern lv_obj_t *dd_chan_behave;
extern lv_obj_t *dd_hardware_input;
extern lv_obj_t *lbl_hw_status_val;
extern lv_obj_t *sw_invert;
extern lv_obj_t *sw_behave_is_chan;
extern lv_obj_t *ta_states; // NEU: Multi-State Zustaende Textfeld
extern lv_obj_t *sb_trim;
extern lv_obj_t *sb_const_min;
extern lv_obj_t *sb_const_max;
extern lv_obj_t *sb_deadband;
extern lv_obj_t *sb_damp_fwd;
extern lv_obj_t *sb_damp_ret;
extern lv_obj_t *dd_curve_type;
extern lv_obj_t *sl_curve_gain;
extern lv_obj_t *lbl_curve_gain_val;
extern lv_obj_t *dd_failsafe_type;
extern lv_obj_t *ta_failsafe_val;

#endif // CHANNEL_SUBPAGE_H