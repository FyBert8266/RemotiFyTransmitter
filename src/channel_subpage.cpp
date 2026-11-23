/**
 * @file channel_subpage.cpp
 * @version 1.0.4
 * @brief Implementierung der Detail-Konfiguration mit Multi-State Zustaende-Textfeld, korrekten Failsafe-Koordinaten und Heap-Diagnose (v1.0.4).
 * @Version_info v1.0.4: Laufzeitgesteuerte Diagnosen ueber SerialInterface; Ersetzung von lv_obj_set_hidden() durch LVGL-9-konforme Flags (LV_OBJ_FLAG_HIDDEN); sicheres Kanal-Loeschen und -Sichern via lv_async_call mit Bounds-Checks und Sync auf pairedModels[conn_idx].config bei aktiver Verbindung.
 *               v1.0.3: Hinzufuegen von ta_states fÃ¼r Multi-State KanÃ¤le in Kopfdaten sowie prÃ¤zise Grid-Positionierung im Failsafe-Tab.
 */


#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include "SerialInterface.h"
#include "config_types.h"
#include "hardware_io.h"
#include "model_page.h"
#include "channel_subpage.h"
#include "dashboard_page.h"

// ============================================================
// GLOBALE STATUSVARIABLEN & UI-REFERENZEN
// ============================================================

int active_channel_idx = -1;
bool is_config_page_open = false;

lv_obj_t *channel_config_subpage = NULL;
lv_obj_t *ta_chan_name = NULL;
lv_obj_t *dd_chan_type = NULL;
lv_obj_t *dd_chan_behave = NULL;
lv_obj_t *dd_hardware_input = NULL;
lv_obj_t *lbl_hw_status_val = NULL;
lv_obj_t *sw_invert = NULL;
lv_obj_t *sw_behave_is_chan = NULL;
lv_obj_t *ta_states = NULL; // NEU
lv_obj_t *sb_trim = NULL;
lv_obj_t *sb_const_min = NULL;
lv_obj_t *sb_const_max = NULL;
lv_obj_t *sb_deadband = NULL;
lv_obj_t *sb_damp_fwd = NULL;
lv_obj_t *sb_damp_ret = NULL;
lv_obj_t *dd_curve_type = NULL;
lv_obj_t *sl_curve_gain = NULL;
lv_obj_t *lbl_curve_gain_val = NULL;
lv_obj_t *dd_failsafe_type = NULL;
lv_obj_t *ta_failsafe_val = NULL;

static lv_obj_t *mbox_chan_delete_confirm = NULL;
static lv_obj_t *mbox_cancel_confirm = NULL;
static lv_obj_t *global_kb = NULL;
static lv_obj_t *kb_preview_ta = NULL;
static lv_obj_t *active_target_ta = NULL;
static bool clear_on_next_key = false;

extern PairedModelInfo *pairedModels;
extern int8_t activeModelIndex;

// ============================================================
// PROTOTYPEN & DIAGNOSE-HILFSFUNKTIONEN
// ============================================================

static void mbox_delete_channel_confirm_cb(lv_event_t *e);
static void mbox_cancel_confirm_cb(lv_event_t *e);
static void kb_preview_sync_cb(lv_event_t *e);
static void kb_event_cb(lv_event_t *e);
static void delete_channel_cb(lv_event_t *e);
static void btn_cancel_detail_cb(lv_event_t *e);
static void curve_gain_slider_cb(lv_event_t *e);
static void spinbox_btn_cb(lv_event_t *e);
static void print_memory_status(const char *tag);
static void cleanup_channel_config_page(void);
static void execute_delete_channel_async_cb(void *arg);
static void execute_cancel_detail_async_cb(void *arg);
static void execute_save_and_back_async_cb(void *arg);

/**
 * @brief Gibt eine detaillierte Zustands- und Zeigerdiagnose der channel_subpage aus.
 * @param step Bezeichner des aktuellen Ausfuehrungsschritts.
 */
static void log_channel_subpage_state(const char *step) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  SerialInterface::printDebug(SerialInterface::Module::Channels, "[CH_SUB][TRACE] [%s] | active_ch=%d | chOpen=%d | chCnt=%d | subpage=%p(valid=%d) | kb=%p(valid=%d) | prev_ta=%p(valid=%d) | target_ta=%p\n",
                step,
                active_channel_idx,
                (int)is_config_page_open,
                (int)currentConfig.channelCount,
                (void *)channel_config_subpage,
                (channel_config_subpage != NULL) ? (int)lv_obj_is_valid(channel_config_subpage) : 0,
                (void *)global_kb,
                (global_kb != NULL) ? (int)lv_obj_is_valid(global_kb) : 0,
                (void *)kb_preview_ta,
                (kb_preview_ta != NULL) ? (int)lv_obj_is_valid(kb_preview_ta) : 0,
                (void *)active_target_ta);
  
  }
}

/**
 * @brief Gibt den aktuellen Heap- und VRAM-Status im seriellen Monitor aus.
 * @param tag Bezeichner fuer die Debug-Ausgabe.
 */
static void print_memory_status(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  uint32_t free_heap = ESP.getFreeHeap();
  uint32_t min_free_heap = ESP.getMinFreeHeap();
  uint32_t max_alloc_heap = ESP.getMaxAllocHeap();
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[MEM DIAGNOSE] [%s] Freier Heap: %u Bytes | Min Frei: %u Bytes | Groesster Block: %u Bytes\n", 
                tag, free_heap, min_free_heap, max_alloc_heap);
  
  }

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

/**
 * @brief Synchronisiert currentConfig bei aktiver Modellverbindung in pairedModels[conn_idx].config
 *        und setzt den Dirty-Marker ("Modelle *").
 */
static void sync_channel_change_to_connected_model(void) {
  int conn_idx = get_connected_paired_model_index();
  if (conn_idx >= 0 && pairedModels != NULL && conn_idx < MAX_PAIRED_MODELS) {
    pairedModels[conn_idx].config = currentConfig;
    mark_paired_model_config_dirty(conn_idx, true);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
    SerialInterface::printDebug(SerialInterface::Module::Channels, "[CH_SUB][TRACE] Kanal-Aenderung in pairedModels[%d].config uebernommen (chCnt=%d, dirty=JA).\n",
                  conn_idx, (int)currentConfig.channelCount);
    
  }
  }
}

#pragma region CLEANUP_AND_DIALOGS

/**
 * @brief Raeumt alle UI-Ressourcen der Kanal-Konfigurationsseite vollstaendig vom Heap ab.
 */
static void cleanup_channel_config_page(void) {
  log_channel_subpage_state("cleanup_channel_config_page:ENTER");
  is_config_page_open = false;
  active_target_ta = NULL;

  if (global_kb && lv_obj_is_valid(global_kb)) {
    lv_obj_delete(global_kb);
  }
  global_kb = NULL;

  if (kb_preview_ta && lv_obj_is_valid(kb_preview_ta)) {
    lv_obj_delete(kb_preview_ta);
  }
  kb_preview_ta = NULL;

  if (channel_config_subpage && lv_obj_is_valid(channel_config_subpage)) {
    lv_obj_delete(channel_config_subpage);
  }
  channel_config_subpage = NULL;

  ta_chan_name = NULL;
  dd_chan_type = NULL;
  dd_chan_behave = NULL;
  dd_hardware_input = NULL;
  lbl_hw_status_val = NULL;
  sw_invert = NULL;
  sw_behave_is_chan = NULL;
  ta_states = NULL;
  sb_trim = NULL;
  sb_const_min = NULL;
  sb_const_max = NULL;
  sb_deadband = NULL;
  sb_damp_fwd = NULL;
  sb_damp_ret = NULL;
  dd_curve_type = NULL;
  sl_curve_gain = NULL;
  lbl_curve_gain_val = NULL;
  dd_failsafe_type = NULL;
  ta_failsafe_val = NULL;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  SerialInterface::printDebug(SerialInterface::Module::Channels, "[DEBUG] Kanal-Konfigurationsseite vollstaendig vom Heap bereinigt.");
  
  }
  print_memory_status("Cleanup Ende");
}

static void delete_channel_cb(lv_event_t *e) {
  (void)e;
  log_channel_subpage_state("delete_channel_cb:ENTER");
  if (mbox_chan_delete_confirm && lv_obj_is_valid(mbox_chan_delete_confirm)) {
    lv_msgbox_close(mbox_chan_delete_confirm);
  }
  mbox_chan_delete_confirm = NULL;

  if (!tab_configs || !lv_obj_is_valid(tab_configs)) return;

  mbox_chan_delete_confirm = lv_msgbox_create(tab_configs);
  if (!mbox_chan_delete_confirm) return;

  lv_msgbox_add_title(mbox_chan_delete_confirm, "Kanal loeschen?");

  char buf[80];
  if (active_channel_idx >= 0 && active_channel_idx < (int)currentConfig.channelCount && active_channel_idx < MAX_CHANNELS) {
    snprintf(buf, sizeof(buf), "Moechtest du den Kanal '%s' wirklich unwiderruflich loeschen?", currentConfig.channels[active_channel_idx].name);
  } else {
    snprintf(buf, sizeof(buf), "Moechtest du diesen Kanal wirklich loeschen?");
  }
  lv_msgbox_add_text(mbox_chan_delete_confirm, buf);

  lv_obj_t *b_del = lv_msgbox_add_footer_button(mbox_chan_delete_confirm, "Loeschen");
  lv_obj_t *b_can = lv_msgbox_add_footer_button(mbox_chan_delete_confirm, "Abbrechen");

  if (b_del && b_can) {
    lv_obj_set_style_bg_color(b_del, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(b_del, mbox_delete_channel_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_can, mbox_delete_channel_confirm_cb, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_center(mbox_chan_delete_confirm);
}

static void btn_cancel_detail_cb(lv_event_t *e) {
  (void)e;
  log_channel_subpage_state("btn_cancel_detail_cb:ENTER");
  if (mbox_cancel_confirm && lv_obj_is_valid(mbox_cancel_confirm)) {
    lv_msgbox_close(mbox_cancel_confirm);
  }
  mbox_cancel_confirm = NULL;

  if (!tab_configs || !lv_obj_is_valid(tab_configs)) return;

  mbox_cancel_confirm = lv_msgbox_create(tab_configs);
  if (!mbox_cancel_confirm) return;

  lv_msgbox_add_title(mbox_cancel_confirm, "Aenderungen verwerfen?");
  lv_msgbox_add_text(mbox_cancel_confirm, "Moechtest du die Detailseite wirklich verlassen?\nAlle ungesicherten Aenderungen gehen verloren.");

  lv_obj_t *b_ja = lv_msgbox_add_footer_button(mbox_cancel_confirm, "Ja");
  lv_obj_t *b_nein = lv_msgbox_add_footer_button(mbox_cancel_confirm, "Nein");

  if (b_ja && b_nein) {
    lv_obj_set_style_bg_color(b_ja, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(b_ja, mbox_cancel_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b_nein, mbox_cancel_confirm_cb, LV_EVENT_CLICKED, NULL);
  }

  lv_obj_center(mbox_cancel_confirm);
}

static void execute_cancel_detail_async_cb(void *arg) {
  (void)arg;
  log_channel_subpage_state("execute_cancel_detail_async_cb:ENTER");
  cleanup_channel_config_page();
  build_channel_list_page();
  print_memory_status("Abbrechen & Schliessen");
}

static void mbox_cancel_confirm_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));
    bool do_cancel = (btn_txt && strcmp(btn_txt, "Ja") == 0);

    if (mbox_cancel_confirm && lv_obj_is_valid(mbox_cancel_confirm)) {
      lv_msgbox_close(mbox_cancel_confirm);
    }
    mbox_cancel_confirm = NULL;

    if (do_cancel) {
      lv_async_call(execute_cancel_detail_async_cb, NULL);
    }
  }
}

static void execute_delete_channel_async_cb(void *arg) {
  int del_idx = (int)(intptr_t)arg;
  log_channel_subpage_state("execute_delete_channel_async_cb:ENTER");
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  SerialInterface::printDebug(SerialInterface::Module::Channels, "[CH_SUB][TRACE] Loesche Kanal-Index=%d (vorher channelCount=%d)\n",
                del_idx, (int)currentConfig.channelCount);
  
  }

  if (del_idx >= 0 && del_idx < (int)currentConfig.channelCount && del_idx < MAX_CHANNELS && currentConfig.channelCount > 0) {
    for (int i = del_idx; i < (int)currentConfig.channelCount - 1 && i < MAX_CHANNELS - 1; i++) {
      currentConfig.channels[i] = currentConfig.channels[i + 1];
    }
    if (currentConfig.channelCount - 1 >= 0 && currentConfig.channelCount - 1 < MAX_CHANNELS) {
      memset(&currentConfig.channels[currentConfig.channelCount - 1], 0, sizeof(ChannelConfig));
    }
    currentConfig.channelCount--;
    sync_channel_change_to_connected_model();
  }

  active_channel_idx = -1;
  cleanup_channel_config_page();
  build_channel_list_page();
  check_config_changes();
  print_memory_status("Kanal Loeschen");
}

static void mbox_delete_channel_confirm_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    const char *btn_txt = lv_label_get_text((lv_obj_t *)lv_obj_get_child(btn, 0));
    bool execute_delete = (btn_txt && strcmp(btn_txt, "Loeschen") == 0);
    int idx_to_delete = active_channel_idx;

    if (mbox_chan_delete_confirm && lv_obj_is_valid(mbox_chan_delete_confirm)) {
      lv_msgbox_close(mbox_chan_delete_confirm);
    }
    mbox_chan_delete_confirm = NULL;

    if (execute_delete) {
      lv_async_call(execute_delete_channel_async_cb, (void *)(intptr_t)idx_to_delete);
    }
  }
}

#pragma endregion

#pragma region KEYBOARD_AND_CALLBACKS

static void curve_gain_slider_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    int32_t val = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    if (lbl_curve_gain_val && lv_obj_is_valid(lbl_curve_gain_val)) {
      lv_label_set_text_fmt(lbl_curve_gain_val, "%d%%", (int)val);
    }
  }
}

static void kb_preview_sync_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED && kb_preview_ta && active_target_ta &&
      lv_obj_is_valid(kb_preview_ta) && lv_obj_is_valid(active_target_ta)) {
    const char *current_text = lv_textarea_get_text(kb_preview_ta);
    if (clear_on_next_key && strlen(current_text) > 0) {
      clear_on_next_key = false;
      char last_char[2] = { current_text[strlen(current_text) - 1], '\0' };
      lv_textarea_set_text(kb_preview_ta, last_char);
      current_text = lv_textarea_get_text(kb_preview_ta);
    }

    if (active_target_ta == ta_chan_name || active_target_ta == ta_model_name || active_target_ta == ta_states) {
      lv_textarea_set_text(active_target_ta, current_text);
      if (active_target_ta == ta_model_name) {
        snprintf(currentConfig.modelName, MAX_NAME_LEN, "%s", current_text);
      }
      return;
    }
    
    int min_limit = -100, max_limit = 100;
    if (active_target_ta == sb_const_min) {
      min_limit = -100;
      max_limit = 0;
    } else if (active_target_ta == sb_const_max || active_target_ta == sb_damp_fwd || active_target_ta == sb_damp_ret) {
      min_limit = 0;
      max_limit = 100;
    } else if (active_target_ta == ta_failsafe_val) {
      min_limit = 0;
      max_limit = 2000;
    }
    
    int val = atoi(current_text);
    if (strlen(current_text) > 0 && strcmp(current_text, "-") != 0) {
      if (val < min_limit) val = min_limit;
      if (val > max_limit) val = max_limit;
    }
    
    if (active_target_ta != ta_failsafe_val) {
      if (lv_spinbox_get_value(active_target_ta) != val) {
        lv_spinbox_set_value(active_target_ta, val);
      }
    } else {
      lv_textarea_set_text(active_target_ta, current_text);
    }
  }
}

static void kb_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (kb && lv_obj_is_valid(kb)) {
      lv_obj_set_hidden(kb, true);
    }
    if (kb_preview_ta && lv_obj_is_valid(kb_preview_ta)) {
      lv_obj_set_hidden(kb_preview_ta, true);
    }
    if (active_target_ta && lv_obj_is_valid(active_target_ta)) {
      lv_obj_clear_state(active_target_ta, LV_STATE_FOCUSED);
    }
    active_target_ta = NULL;
  }
}

void ta_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_FOCUSED && ta && lv_obj_is_valid(ta)) {
    active_target_ta = ta;
    clear_on_next_key = false;

    if (global_kb == NULL || !lv_obj_is_valid(global_kb)) {
      global_kb = lv_keyboard_create(lv_screen_active());
      lv_obj_add_event_cb(global_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    }
    if (kb_preview_ta == NULL || !lv_obj_is_valid(kb_preview_ta)) {
      kb_preview_ta = lv_textarea_create(lv_screen_active());
      lv_textarea_set_one_line(kb_preview_ta, true);
      lv_obj_set_size(kb_preview_ta, 240, 32);
      lv_obj_add_event_cb(kb_preview_ta, kb_preview_sync_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    if (ta != ta_chan_name && ta != ta_model_name && ta != ta_states) {
      lv_keyboard_set_mode(global_kb, LV_KEYBOARD_MODE_NUMBER);
    } else {
      lv_keyboard_set_mode(global_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }

    lv_keyboard_set_textarea(global_kb, kb_preview_ta);
    lv_textarea_set_text(kb_preview_ta, lv_textarea_get_text(ta));

    lv_textarea_set_cursor_pos(kb_preview_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);

    lv_obj_set_hidden(global_kb, false);
    lv_obj_set_hidden(kb_preview_ta, false);
    lv_obj_align_to(kb_preview_ta, global_kb, LV_ALIGN_OUT_TOP_MID, 0, -2);
    lv_obj_add_state(kb_preview_ta, LV_STATE_FOCUSED);
  }
}

static void spinbox_btn_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  intptr_t btn_type = (intptr_t)lv_event_get_user_data(e);

  if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
    lv_obj_t *container = lv_obj_get_parent(btn);
    lv_obj_t *sb = container ? lv_obj_get_child(container, 1) : NULL;
    if (sb && lv_obj_is_valid(sb)) {
      int32_t val = lv_spinbox_get_value(sb);
      if (btn_type == 1) val++;
      else if (btn_type == 2) val--;
      lv_spinbox_set_value(sb, val);
      lv_obj_send_event(sb, LV_EVENT_VALUE_CHANGED, NULL);
    }
  }
}

static void execute_save_and_back_async_cb(void *arg) {
  (void)arg;
  log_channel_subpage_state("execute_save_and_back_async_cb:ENTER");
  cleanup_channel_config_page();
  rebuild_dynamic_gear_bar();
  build_channel_list_page();
  check_config_changes();

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  SerialInterface::printDebug(SerialInterface::Module::Channels, "[DEBUG] Kanal-Aenderungen gesichert und zurueck zur Kanalliste.");
  
  }
  print_memory_status("Sichern & Schliessen");
}

static void back_and_save_cb(lv_event_t *e) {
  (void)e;
  log_channel_subpage_state("back_and_save_cb:ENTER");
  if (active_channel_idx >= 0 && active_channel_idx < (int)currentConfig.channelCount && active_channel_idx < MAX_CHANNELS) {
    ChannelConfig &chan = currentConfig.channels[active_channel_idx];
    
    // Kopfdaten & Multi-State Zustaende uebernehmen
    if (ta_chan_name && lv_obj_is_valid(ta_chan_name)) snprintf(chan.name, MAX_NAME_LEN, "%s", lv_textarea_get_text(ta_chan_name));
    if (dd_chan_type && lv_obj_is_valid(dd_chan_type)) chan.type = lv_dropdown_get_selected(dd_chan_type);
    if (dd_hardware_input && lv_obj_is_valid(dd_hardware_input)) chan.hardwareInputChannel = lv_dropdown_get_selected(dd_hardware_input);
    if (ta_states && lv_obj_is_valid(ta_states)) snprintf(chan.states, sizeof(chan.states), "%s", lv_textarea_get_text(ta_states));
    
    // Verhalten uebernehmen
    if (dd_chan_behave && lv_obj_is_valid(dd_chan_behave)) chan.behaviour = lv_dropdown_get_selected(dd_chan_behave);
    if (sw_invert && lv_obj_is_valid(sw_invert)) chan.invert = lv_obj_has_state(sw_invert, LV_STATE_CHECKED);
    if (sw_behave_is_chan && lv_obj_is_valid(sw_behave_is_chan)) chan.behaviourIsChannel = lv_obj_has_state(sw_behave_is_chan, LV_STATE_CHECKED);
    
    // Trim & Limits uebernehmen
    if (sb_trim && lv_obj_is_valid(sb_trim)) chan.trim = (int16_t)lv_spinbox_get_value(sb_trim);
    if (sb_const_min && lv_obj_is_valid(sb_const_min)) chan.constrainMin = (int16_t)lv_spinbox_get_value(sb_const_min);
    if (sb_const_max && lv_obj_is_valid(sb_const_max)) chan.constrainMax = (int16_t)lv_spinbox_get_value(sb_const_max);
    if (sb_deadband && lv_obj_is_valid(sb_deadband)) chan.deadband = (uint8_t)lv_spinbox_get_value(sb_deadband);
    
    // Daempfung uebernehmen
    if (sb_damp_fwd && lv_obj_is_valid(sb_damp_fwd)) chan.dampingForward = (uint8_t)lv_spinbox_get_value(sb_damp_fwd);
    if (sb_damp_ret && lv_obj_is_valid(sb_damp_ret)) chan.dampingReturn = (uint8_t)lv_spinbox_get_value(sb_damp_ret);
    
    // Kurven uebernehmen
    if (dd_curve_type && lv_obj_is_valid(dd_curve_type)) chan.curveType = lv_dropdown_get_selected(dd_curve_type);
    if (sl_curve_gain && lv_obj_is_valid(sl_curve_gain)) chan.curveGain = lv_slider_get_value(sl_curve_gain);
    
    // Failsafe uebernehmen
    if (dd_failsafe_type && lv_obj_is_valid(dd_failsafe_type)) chan.failsafeType = lv_dropdown_get_selected(dd_failsafe_type);
    if (ta_failsafe_val && lv_obj_is_valid(ta_failsafe_val)) chan.failsafeValue = (int16_t)atoi(lv_textarea_get_text(ta_failsafe_val));

    sync_channel_change_to_connected_model();
  }

  lv_async_call(execute_save_and_back_async_cb, NULL);
}

#pragma endregion

#pragma region PAGE_BUILD

/**
 * @brief Erstellt die vollstaendige Kanal-Konfigurationsseite mit allen Unter-Tabs.
 * @param channel_idx Index des zu konfigurierenden Kanals.
 */
void open_channel_config_page(int channel_idx) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  SerialInterface::printDebug(SerialInterface::Module::Channels, "[CH_SUB][TRACE] open_channel_config_page:ENTER(channel_idx=%d, channelCount=%d)\n",
                channel_idx, (int)currentConfig.channelCount);
  
  }
  log_channel_subpage_state("open_channel_config_page:BEFORE_CHECKS");

  if (!tab_configs || !lv_obj_is_valid(tab_configs)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
    SerialInterface::printDebug(SerialInterface::Module::Channels, "[DEBUG] Fehler: tab_configs ist ungueltig!");
    
  }
    return;
  }

  if (channel_idx < 0 || channel_idx >= (int)currentConfig.channelCount || channel_idx >= MAX_CHANNELS) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
    SerialInterface::printDebug(SerialInterface::Module::Channels, "[CH_SUB][ERROR] open_channel_config_page abgebrochen: ungueltiger channel_idx=%d (count=%d, max=%d)\n",
                  channel_idx, (int)currentConfig.channelCount, MAX_CHANNELS);
    
  }
    return;
  }

  active_channel_idx = channel_idx;
  ChannelConfig &chan = currentConfig.channels[channel_idx];
  
  if (channel_list_subpage && lv_obj_is_valid(channel_list_subpage)) {
    lv_obj_delete(channel_list_subpage);
  }
  channel_list_subpage = NULL;

  if (channel_config_subpage && lv_obj_is_valid(channel_config_subpage)) {
    lv_obj_delete(channel_config_subpage);
  }
  channel_config_subpage = NULL;
  
  lv_obj_set_style_pad_all(tab_configs, 0, 0);
  lv_obj_set_style_border_width(tab_configs, 0, 0);

  channel_config_subpage = lv_obj_create(tab_configs);
  if (!channel_config_subpage) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
    SerialInterface::printDebug(SerialInterface::Module::Channels, "[CH_SUB][ERROR] lv_obj_create(tab_configs) fuer channel_config_subpage lieferte NULL!");
    
  }
    return;
  }

  lv_obj_set_size(channel_config_subpage, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(channel_config_subpage, 0, 0);
  lv_obj_set_style_border_width(channel_config_subpage, 0, 0);
  lv_obj_set_scrollable(channel_config_subpage, false);

  static int32_t page_col_dsc[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
  static int32_t page_row_dsc[] = { 25, LV_GRID_FR(1), 35, LV_GRID_TEMPLATE_LAST };
  lv_obj_set_grid_dsc_array(channel_config_subpage, page_col_dsc, page_row_dsc);
  lv_obj_set_style_pad_row(channel_config_subpage, 0, 0);

  // OBEN: Infobanner
  lv_obj_t *info_banner = lv_obj_create(channel_config_subpage);
  lv_obj_set_grid_cell(info_banner, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
  lv_obj_set_style_bg_color(info_banner, lv_palette_darken(LV_PALETTE_BLUE, 3), 0);
  lv_obj_set_style_border_width(info_banner, 0, 0);
  lv_obj_set_style_radius(info_banner, 0, 0);
  lv_obj_set_style_pad_all(info_banner, 0, 0);
  lv_obj_set_scrollable(info_banner, false);

  lv_obj_t *lbl_info_title = lv_label_create(info_banner);
  lv_label_set_text_fmt(lbl_info_title, "Kanal-Konfiguration: CH %d [%s]", channel_idx + 1, chan.name);
  lv_obj_set_style_text_font(lbl_info_title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_info_title, lv_color_white(), 0);
  lv_obj_align(lbl_info_title, LV_ALIGN_CENTER, 0, 0);

  // MITTE: Tabview mit allen 6 Unterseiten
  lv_obj_t *tabview_setup = lv_tabview_create(channel_config_subpage);
  lv_tabview_set_tab_bar_position(tabview_setup, LV_DIR_LEFT);
  lv_tabview_set_tab_bar_size(tabview_setup, 125);
  lv_obj_set_grid_cell(tabview_setup, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
  lv_obj_set_style_pad_all(tabview_setup, 0, 0);
  lv_obj_set_style_border_width(tabview_setup, 0, 0);

  lv_obj_t *tab_setup_kopfdaten = lv_tabview_add_tab(tabview_setup, "Kopfdaten");
  lv_obj_t *tab_setup_verhalten = lv_tabview_add_tab(tabview_setup, "Verhalten");
  lv_obj_t *tab_setup_trim = lv_tabview_add_tab(tabview_setup, "Trim/Limits");
  lv_obj_t *tab_setup_daempfung = lv_tabview_add_tab(tabview_setup, "Daempfung");
  lv_obj_t *tab_setup_kurven = lv_tabview_add_tab(tabview_setup, "Kurven");
  lv_obj_t *tab_setup_failsafe = lv_tabview_add_tab(tabview_setup, "Failsafe");

  static int32_t col_dsc[] = { LV_PCT(40), LV_PCT(60), LV_GRID_TEMPLATE_LAST };
  static int32_t row_dsc[] = { LV_PCT(20), LV_PCT(20), LV_PCT(20), LV_PCT(20), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

  // --- TAB 1: Kopfdaten (inkl. Multi-State Zustaende) ---
  lv_obj_set_grid_dsc_array(tab_setup_kopfdaten, col_dsc, row_dsc);

  // Zeile 0: Kanal Name
  lv_obj_t *lbl1 = lv_label_create(tab_setup_kopfdaten);
  lv_label_set_text(lbl1, "Kanal Name:");
  lv_obj_set_grid_cell(lbl1, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  ta_chan_name = lv_textarea_create(tab_setup_kopfdaten);
  lv_textarea_set_one_line(ta_chan_name, true);
  lv_textarea_set_text(ta_chan_name, chan.name);
  lv_obj_set_scrollable(ta_chan_name, false);
  lv_obj_set_size(ta_chan_name, 140, 30);
  lv_obj_add_event_cb(ta_chan_name, ta_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_set_style_pad_top(ta_chan_name, 5, 0);
  lv_obj_set_style_pad_bottom(ta_chan_name, 0, 0);
  lv_obj_set_style_pad_left(ta_chan_name, 8, 0);
  lv_obj_set_style_pad_right(ta_chan_name, 8, 0);
  lv_obj_set_style_min_height(ta_chan_name, 0, 0);
  lv_obj_set_grid_cell(ta_chan_name, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  // Zeile 1: Typ
  lv_obj_t *lbl2 = lv_label_create(tab_setup_kopfdaten);
  lv_label_set_text(lbl2, "Typ:");
  lv_obj_set_grid_cell(lbl2, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  dd_chan_type = lv_dropdown_create(tab_setup_kopfdaten);
  lv_dropdown_set_options(dd_chan_type, "Analog\nDigital\nMulti-State");
  lv_dropdown_set_selected(dd_chan_type, chan.type);
  lv_obj_set_size(dd_chan_type, 140, 30);
  lv_obj_set_grid_cell(dd_chan_type, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);
  
  lv_obj_t *list_type = lv_dropdown_get_list(dd_chan_type);
  if (list_type) lv_obj_set_style_max_height(list_type, 110, 0);

  // Zeile 2: Eingang HW
  lv_obj_t *lbl_hw = lv_label_create(tab_setup_kopfdaten);
  lv_label_set_text(lbl_hw, "Eingang HW:");
  lv_obj_set_grid_cell(lbl_hw, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 2, 1);

  dd_hardware_input = lv_dropdown_create(tab_setup_kopfdaten);
  if (NUM_HARDWARE_INPUTS > 0) {
    String hw_options = "";
    for (uint8_t i = 0; i < NUM_HARDWARE_INPUTS; i++) {
      if (AVAILABLE_HARDWARE_INPUTS[i].label != NULL) {
        hw_options += AVAILABLE_HARDWARE_INPUTS[i].label;
      } else {
        hw_options += "Unbekannt";
      }
      if (i < NUM_HARDWARE_INPUTS - 1) hw_options += "\n";
    }
    lv_dropdown_set_options(dd_hardware_input, hw_options.c_str());
  } else {
    lv_dropdown_set_options(dd_hardware_input, "Keine Eingaenge");
  }

  uint8_t selected_input = chan.hardwareInputChannel;
  if (selected_input >= NUM_HARDWARE_INPUTS) selected_input = 0;
  lv_dropdown_set_selected(dd_hardware_input, selected_input);
  lv_obj_set_size(dd_hardware_input, 140, 30);
  lv_obj_set_grid_cell(dd_hardware_input, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 2, 1);
  
  lv_obj_t *hw_list = lv_dropdown_get_list(dd_hardware_input);
  if (hw_list) lv_obj_set_style_max_height(hw_list, 110, 0);

  // Zeile 3: Multi-State Zustaende Textfeld (z.B. fuer Ganganzeige R,N,1,2,3,4,5,6)
  lv_obj_t *lbl_states = lv_label_create(tab_setup_kopfdaten);
  lv_label_set_text(lbl_states, "Zustaende:");
  lv_obj_set_grid_cell(lbl_states, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 3, 1);

  ta_states = lv_textarea_create(tab_setup_kopfdaten);
  lv_textarea_set_one_line(ta_states, true);
  lv_textarea_set_text(ta_states, chan.states);
  lv_obj_set_scrollable(ta_states, false);
  lv_obj_set_size(ta_states, 140, 30);
  lv_obj_add_event_cb(ta_states, ta_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_set_style_pad_top(ta_states, 5, 0);
  lv_obj_set_style_pad_bottom(ta_states, 0, 0);
  lv_obj_set_style_pad_left(ta_states, 8, 0);
  lv_obj_set_style_pad_right(ta_states, 8, 0);
  lv_obj_set_style_min_height(ta_states, 0, 0);
  lv_obj_set_grid_cell(ta_states, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 3, 1);


  // --- TAB 2: Verhalten ---
  lv_obj_set_grid_dsc_array(tab_setup_verhalten, col_dsc, row_dsc);

  lv_obj_t *lbl3 = lv_label_create(tab_setup_verhalten);
  lv_label_set_text(lbl3, "Verhalten:");
  lv_obj_set_grid_cell(lbl3, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  dd_chan_behave = lv_dropdown_create(tab_setup_verhalten);
  lv_dropdown_set_options(dd_chan_behave, "ResZero\nStick\nMomentary\nToggle");
  lv_dropdown_set_selected(dd_chan_behave, chan.behaviour);
  lv_obj_set_size(dd_chan_behave, 140, 30);
  lv_obj_set_grid_cell(dd_chan_behave, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  
  lv_obj_t *list_behave = lv_dropdown_get_list(dd_chan_behave);
  if (list_behave) lv_obj_set_style_max_height(list_behave, 110, 0);

  lv_obj_t *lbl4 = lv_label_create(tab_setup_verhalten);
  lv_label_set_text(lbl4, "Invertieren:");
  lv_obj_set_grid_cell(lbl4, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  sw_invert = lv_switch_create(tab_setup_verhalten);
  if (chan.invert) lv_obj_add_state(sw_invert, LV_STATE_CHECKED);
  lv_obj_set_grid_cell(sw_invert, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  lv_obj_t *lbl5 = lv_label_create(tab_setup_verhalten);
  lv_label_set_text(lbl5, "Behav. is Chan:");
  lv_obj_set_grid_cell(lbl5, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 2, 1);

  sw_behave_is_chan = lv_switch_create(tab_setup_verhalten);
  if (chan.behaviourIsChannel) lv_obj_add_state(sw_behave_is_chan, LV_STATE_CHECKED);
  lv_obj_set_grid_cell(sw_behave_is_chan, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 2, 1);


  // Makro zur komfortablen Erstellung von Detail-Spinboxen mit Plus/Minus Buttons
  #define CREATE_DETAIL_SPINBOX(target_tab, sb_obj, label_text, row, min_v, max_v, curr_v) \
    lv_obj_t *lbl_##sb_obj = lv_label_create(target_tab); \
    lv_label_set_text(lbl_##sb_obj, label_text); \
    lv_obj_set_grid_cell(lbl_##sb_obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1); \
    lv_obj_t *cont_##sb_obj = lv_obj_create(target_tab); \
    lv_obj_set_size(cont_##sb_obj, 140, 32); \
    lv_obj_set_grid_cell(cont_##sb_obj, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, row, 1); \
    lv_obj_set_flex_flow(cont_##sb_obj, LV_FLEX_FLOW_ROW_REVERSE); \
    lv_obj_set_flex_align(cont_##sb_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); \
    lv_obj_set_style_pad_all(cont_##sb_obj, 0, 0); \
    lv_obj_set_style_pad_gap(cont_##sb_obj, 3, 0); \
    lv_obj_set_style_border_width(cont_##sb_obj, 0, 0); \
    lv_obj_set_style_bg_opa(cont_##sb_obj, LV_OPA_TRANSP, 0); \
    lv_obj_set_scrollable(cont_##sb_obj, false); \
    lv_obj_t *btn_pls_##sb_obj = lv_button_create(cont_##sb_obj); \
    lv_obj_set_size(btn_pls_##sb_obj, 26, 26); \
    lv_obj_t *lbl_pls_##sb_obj = lv_label_create(btn_pls_##sb_obj); \
    lv_label_set_text(lbl_pls_##sb_obj, LV_SYMBOL_PLUS); \
    lv_obj_center(lbl_pls_##sb_obj); \
    lv_obj_add_event_cb(btn_pls_##sb_obj, spinbox_btn_cb, LV_EVENT_ALL, (void *)1); \
    sb_obj = lv_spinbox_create(cont_##sb_obj); \
    lv_spinbox_set_range(sb_obj, min_v, max_v); \
    lv_spinbox_set_digit_format(sb_obj, 3, 0); \
    lv_spinbox_set_value(sb_obj, curr_v); \
    lv_obj_set_size(sb_obj, 82, 26); \
    lv_obj_set_scrollable(sb_obj, false); \
    lv_obj_set_style_text_align(sb_obj, LV_TEXT_ALIGN_RIGHT, 0); \
    lv_obj_set_style_pad_top(sb_obj, 2, 0); \
    lv_obj_set_style_pad_bottom(sb_obj, 0, 0); \
    lv_obj_set_style_pad_left(sb_obj, 0, 0); \
    lv_obj_set_style_pad_right(sb_obj, 6, 0); \
    lv_obj_set_style_margin_all(sb_obj, 0, 0); \
    lv_obj_add_event_cb(sb_obj, ta_event_cb, LV_EVENT_ALL, NULL); \
    lv_obj_t *btn_min_##sb_obj = lv_button_create(cont_##sb_obj); \
    lv_obj_set_size(btn_min_##sb_obj, 26, 26); \
    lv_obj_t *lbl_min_##sb_obj = lv_label_create(btn_min_##sb_obj); \
    lv_label_set_text(lbl_min_##sb_obj, LV_SYMBOL_MINUS); \
    lv_obj_center(lbl_min_##sb_obj); \
    lv_obj_add_event_cb(btn_min_##sb_obj, spinbox_btn_cb, LV_EVENT_ALL, (void *)2);

  // --- TAB 3: Trim/Limits ---
  lv_obj_set_grid_dsc_array(tab_setup_trim, col_dsc, row_dsc);
  CREATE_DETAIL_SPINBOX(tab_setup_trim, sb_trim, "Trim:", 0, -100, 100, chan.trim);
  CREATE_DETAIL_SPINBOX(tab_setup_trim, sb_const_min, "Limit-:", 1, -100, 0, chan.constrainMin);
  CREATE_DETAIL_SPINBOX(tab_setup_trim, sb_const_max, "Limit+:", 2, 0, 100, chan.constrainMax);
  CREATE_DETAIL_SPINBOX(tab_setup_trim, sb_deadband, "Totbereich %:", 3, 0, 50, chan.deadband);

  // --- TAB 4: Daempfung ---
  lv_obj_set_grid_dsc_array(tab_setup_daempfung, col_dsc, row_dsc);
  CREATE_DETAIL_SPINBOX(tab_setup_daempfung, sb_damp_fwd, "Damp Fwd:", 0, 0, 100, chan.dampingForward);
  CREATE_DETAIL_SPINBOX(tab_setup_daempfung, sb_damp_ret, "Damp Ret:", 1, 0, 100, chan.dampingReturn);

  #undef CREATE_DETAIL_SPINBOX

  // --- TAB 5: Kurven ---
  lv_obj_set_grid_dsc_array(tab_setup_kurven, col_dsc, row_dsc);

  lv_obj_t *lbl_curv = lv_label_create(tab_setup_kurven);
  lv_label_set_text(lbl_curv, "Kurve:");
  lv_obj_set_grid_cell(lbl_curv, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  
  dd_curve_type = lv_dropdown_create(tab_setup_kurven);
  lv_dropdown_set_options(dd_curve_type, "Linear\nProgressiv\nDegressiv");
  lv_dropdown_set_selected(dd_curve_type, chan.curveType);
  lv_obj_set_size(dd_curve_type, 140, 30);
  lv_obj_set_grid_cell(dd_curve_type, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  
  lv_obj_t *list_curve = lv_dropdown_get_list(dd_curve_type);
  if (list_curve) lv_obj_set_style_max_height(list_curve, 110, 0);

  lv_obj_t *lbl_gain_title = lv_label_create(tab_setup_kurven);
  lv_label_set_text(lbl_gain_title, "Kurven-Faktor:");
  lv_obj_set_grid_cell(lbl_gain_title, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  sl_curve_gain = lv_slider_create(tab_setup_kurven);
  lv_slider_set_range(sl_curve_gain, -100, 100);
  lv_slider_set_value(sl_curve_gain, chan.curveGain, LV_ANIM_OFF);
  lv_obj_set_width(sl_curve_gain, 115);
  lv_obj_set_grid_cell(sl_curve_gain, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  lbl_curve_gain_val = lv_label_create(tab_setup_kurven);
  lv_obj_set_grid_cell(lbl_curve_gain_val, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);
  lv_label_set_text_fmt(lbl_curve_gain_val, "%d%%", (int)chan.curveGain);
  lv_obj_add_event_cb(sl_curve_gain, curve_gain_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);


  // --- TAB 6: Failsafe (KORRIGIERTE KOORDINATEN) ---
  lv_obj_set_grid_dsc_array(tab_setup_failsafe, col_dsc, row_dsc);

  // Zeile 0: Failsafe Typ
  lv_obj_t *lbl_fs = lv_label_create(tab_setup_failsafe);
  lv_label_set_text(lbl_fs, "Failsafe:");
  lv_obj_set_grid_cell(lbl_fs, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  dd_failsafe_type = lv_dropdown_create(tab_setup_failsafe);
  lv_dropdown_set_options(dd_failsafe_type, "HOLD\nWert");
  lv_dropdown_set_selected(dd_failsafe_type, chan.failsafeType);
  lv_obj_set_size(dd_failsafe_type, 140, 30);
  lv_obj_set_grid_cell(dd_failsafe_type, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  
  lv_obj_t *list_fs = lv_dropdown_get_list(dd_failsafe_type);
  if (list_fs) lv_obj_set_style_max_height(list_fs, 110, 0);

  // Zeile 1: Failsafe Wert
  lv_obj_t *lbl_fs_val = lv_label_create(tab_setup_failsafe);
  lv_label_set_text(lbl_fs_val, "FS Wert:");
  lv_obj_set_grid_cell(lbl_fs_val, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

  ta_failsafe_val = lv_textarea_create(tab_setup_failsafe);
  lv_textarea_set_one_line(ta_failsafe_val, true);
  char fs_buf[8];
  snprintf(fs_buf, sizeof(fs_buf), "%d", chan.failsafeValue);
  lv_textarea_set_text(ta_failsafe_val, fs_buf);
  lv_obj_set_size(ta_failsafe_val, 140, 30);
  lv_obj_set_style_pad_top(ta_failsafe_val, 5, 0);
  lv_obj_set_style_pad_bottom(ta_failsafe_val, 0, 0);
  lv_obj_set_style_pad_left(ta_failsafe_val, 8, 0);
  lv_obj_set_style_pad_right(ta_failsafe_val, 8, 0);
  lv_obj_set_style_min_height(ta_failsafe_val, 0, 0);
  lv_obj_add_event_cb(ta_failsafe_val, ta_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_set_grid_cell(ta_failsafe_val, LV_GRID_ALIGN_END, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);


  // --- UNTEN: Fixierte Aktionsleiste ---
  lv_obj_t *cont_actions = lv_obj_create(channel_config_subpage);
  lv_obj_set_grid_cell(cont_actions, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
  lv_obj_set_flex_flow(cont_actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_actions, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(cont_actions, 0, 0);
  lv_obj_set_style_border_width(cont_actions, 0, 0);
  lv_obj_set_style_radius(cont_actions, 0, 0);
  lv_obj_set_style_bg_color(cont_actions, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
  lv_obj_set_scrollable(cont_actions, false);

  lv_obj_t *back_btn = lv_button_create(cont_actions);
  lv_obj_set_size(back_btn, 130, 28);
  lv_obj_set_style_bg_color(back_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(back_btn, back_and_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_OK " Sichern");
  lv_obj_center(back_lbl);

  lv_obj_t *cancel_btn = lv_button_create(cont_actions);
  lv_obj_set_size(cancel_btn, 130, 28);
  lv_obj_set_style_bg_color(cancel_btn, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_add_event_cb(cancel_btn, btn_cancel_detail_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE " Abbrechen");
  lv_obj_center(cancel_lbl);

  lv_obj_t *del_btn = lv_button_create(cont_actions);
  lv_obj_set_size(del_btn, 130, 28);
  lv_obj_set_style_bg_color(del_btn, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_add_event_cb(del_btn, delete_channel_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *del_lbl = lv_label_create(del_btn);
  lv_label_set_text(del_lbl, LV_SYMBOL_TRASH " Loeschen");
  lv_obj_center(del_lbl);

  is_config_page_open = true;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  SerialInterface::printDebug(SerialInterface::Module::Channels, "[DEBUG] Vollstaendige Kanal-Konfigurationsseite fuer CH %d geoeffnet.\n", channel_idx + 1);
  
  }
  print_memory_status("Oeffnen Seite");
}

#pragma endregion