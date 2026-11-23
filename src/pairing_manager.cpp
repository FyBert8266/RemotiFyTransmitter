/**
 * @file pairing_manager_2.cpp
 * @version 1.2.1
 * @brief Implementierung des LittleFS-Persistenz-Managers (inkl. persistenter Sicherung von pairedModels[i].config),
 *        30s-Pairing-Fensters und thread-sicherer Popups.
 * @Version_info v1.2.1: LittleFS-Mount-Pruefung (ensure_littlefs_ready), sicheres Schliessen aller File-Handles
 *               sowie Schutz gegen redundantes Ueberschreiben bei unveraenderten Slot-Configs.
 *               v1.2.0: Persistentes Speichern und Laden von pairedModels[i].config je Slot ueber /paired_cfg_<idx>.bin;
 *               einheitliche [VRAM DIAGNOSE]-Ausgabe (Auslastung + groesster freier Block).
 */


#include <Arduino.h>
#include <LittleFS.h>
#include <esp_now.h>
#include <lvgl.h>
#include <stdio.h>
#include "pairing_manager.h"
#include "espnow_comm.h"
#include "SerialInterface.h"
#include "audio_module.h"

#pragma region GLOBALS_AND_STATICS
extern PairedModelInfo *pairedModels;
extern int8_t pairedModelCount;
extern int8_t activeModelIndex;
extern SystemCalibrationConfig sysCalibration;

static bool pairingWindowActive = false;
unsigned long pairingWindowStartTime = 0;
static bool pendingConfirmationOpen = false;
static volatile bool requestShowPopup = false;
static uint8_t pendingMac[6] = {0};
static char pendingModelName[MAX_NAME_LEN] = {0};
static lv_obj_t *popup_pairing_win = NULL;

extern void refresh_models_list_ui(void);

static void build_slot_config_filename(int slotIdx, char *outBuf, size_t outLen) {
  snprintf(outBuf, outLen, "/paired_cfg_%d.bin", slotIdx);
}

/**
 * @brief Stellt sicher, dass LittleFS gemountet ist, bevor Dateioperationen ausgefuehrt werden.
 * @return true, wenn LittleFS einsatzbereit ist, sonst false.
 */
static bool ensure_littlefs_ready(void) {
  if (LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
    return true;
  }
  if (LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    return true;
  }
  SerialInterface::printError("[PAIRING][FS-ERR] LittleFS konnte nicht gemountet werden!");
  return false;
}
#pragma endregion

#pragma region INTERNAL_VRAM_DIAGNOSE
/**
 * @brief Gibt den aktuellen LVGL-VRAM-Status im seriellen Monitor aus (zur Speicherdiagnose).
 * @param tag Bezeichner fuer den Aufrufkontext.
 */
static void print_vram_status_pairing(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Groesster freier Block: %u Bytes\n",
                tag, (int)mon.used_pct, (unsigned int)mon.free_biggest_size);
  }
}
#pragma endregion

#pragma region POPUP_CALLBACKS
/**
 * @brief Schliesst das aktive Pairing-Bestaetigungsfenster und gibt den zugewiesenen Speicher frei.
 */
static void close_pairing_popup(void) {
  if (popup_pairing_win != NULL && lv_obj_is_valid(popup_pairing_win)) {
    lv_obj_delete(popup_pairing_win);
  }
  popup_pairing_win = NULL;
  pendingConfirmationOpen = false;
  requestShowPopup = false;
  print_vram_status_pairing("CLOSE_PAIRING_POPUP");
}

/**
 * @brief Event-Callback bei Klick auf "Koppeln" im Bestaetigungs-Popup. Speichert den Peer und aktualisiert das System.
 * @param e Zeiger auf das ausgeloeste LVGL-Ereignis.
 */
static void btn_confirm_pair_cb(lv_event_t *e) {
  (void)e;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING][STEP 1] btn_confirm_pair_cb aufgerufen. pendingConfirmationOpen=%d",
                               pendingConfirmationOpen ? 1 : 0);
  }

  if (!pendingConfirmationOpen) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
    SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING][STEP 1-ABORT] pendingConfirmationOpen ist false -> Abbruch.");
  }
    return;
  }

  if (!ensure_paired_models_allocated()) {
    SerialInterface::printError("[PAIRING][STEP 2-ERR] pairedModels ist NULL / nicht alloziert! Schliesse Popup und breche ab.");
    close_pairing_popup();
    return;
  }

  int targetSlot = -1;
  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (!pairedModels[i].isPaired) {
      targetSlot = i;
      break;
    }
  }

  if (targetSlot >= 0 && targetSlot < MAX_PAIRED_MODELS) {
    memset(&pairedModels[targetSlot], 0, sizeof(PairedModelInfo));
    memcpy(pairedModels[targetSlot].macAddress, pendingMac, 6);

    size_t pendingNameLen = strnlen(pendingModelName, MAX_NAME_LEN - 1);
    if (pendingNameLen > 0) {
      strncpy(pairedModels[targetSlot].modelName, pendingModelName, MAX_NAME_LEN - 1);
    } else {
      snprintf(pairedModels[targetSlot].modelName, MAX_NAME_LEN, "Modell_%d", targetSlot + 1);
    }
    pairedModels[targetSlot].modelName[MAX_NAME_LEN - 1] = '\0';

    pairedModels[targetSlot].isPaired = true;
    pairedModels[targetSlot].sortOrder = (uint8_t)targetSlot;
    pairedModels[targetSlot].lastTelemetryTime = millis();

    int count = 0;
    for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
      if (pairedModels[i].isPaired) count = i + 1;
    }
    pairedModelCount = (int8_t)count;

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, pendingMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_is_peer_exist(pendingMac)) {
      esp_now_del_peer(pendingMac);
    }
    esp_now_add_peer(&peerInfo);

    save_paired_models_to_fs();
    trigger_audio_feedback(AUDIO_EVENT_MODEL_SWITCH);

    PairingMessage pMsg;
    memset(&pMsg, 0, sizeof(PairingMessage));
    pMsg.packetType = MSG_TYPE_PAIRING;
    memcpy(pMsg.receiverMAC, pendingMac, 6);
    strncpy(pMsg.modelName, pairedModels[targetSlot].modelName, MAX_NAME_LEN - 1);
    pMsg.modelName[MAX_NAME_LEN - 1] = '\0';

    size_t senderNameLen = strnlen(sysCalibration.senderName, sizeof(sysCalibration.senderName) - 1);
    const char *sName = (senderNameLen > 0) ? sysCalibration.senderName : "RemotiFySender";
    strncpy(pMsg.senderName, sName, sizeof(pMsg.senderName) - 1);
    pMsg.senderName[sizeof(pMsg.senderName) - 1] = '\0';

    esp_now_send(pendingMac, (uint8_t *)&pMsg, sizeof(PairingMessage));
    request_model_config_from_peer(pendingMac);

    close_pairing_popup();
    refresh_models_list_ui();

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
    SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] Modell '%s' erfolgreich gekoppelt und in LittleFS gespeichert.",
                                 pairedModels[targetSlot].modelName);
  }
  } else {
    SerialInterface::printError("[PAIRING][ERR-FULL] Modellspeicher voll (max. %d)! Schliesse Popup.", MAX_PAIRED_MODELS);
    close_pairing_popup();
  }
}

/**
 * @brief Event-Callback bei Klick auf "Ignorieren / Abbrechen" im Bestaetigungs-Popup.
 * @param e Zeiger auf das ausgeloeste LVGL-Ereignis.
 */
static void btn_reject_pair_cb(lv_event_t *e) {
  (void)e;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] Kopplungsanfrage durch Nutzer abgelehnt.");
  }
  trigger_audio_feedback(AUDIO_EVENT_CLICK);
  close_pairing_popup();
}

/**
 * @brief Erzeugt das grafische Bestaetigungs-Popup als LVGL-Fenster auf dem CYD-Bildschirm.
 */
static void show_pairing_confirmation_popup(void) {
  if (popup_pairing_win != NULL) return;

  popup_pairing_win = lv_obj_create(lv_screen_active());
  lv_obj_set_size(popup_pairing_win, 340, 175);
  lv_obj_center(popup_pairing_win);
  lv_obj_set_style_radius(popup_pairing_win, 0, 0);
  lv_obj_set_style_border_width(popup_pairing_win, 2, 0);
  lv_obj_set_style_border_color(popup_pairing_win, lv_palette_main(LV_PALETTE_ORANGE), 0);
  lv_obj_set_style_bg_color(popup_pairing_win, lv_color_hex(0x161616), 0);
  lv_obj_set_flex_flow(popup_pairing_win, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(popup_pairing_win, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(popup_pairing_win, 10, 0);
  lv_obj_set_scrollable(popup_pairing_win, false);

  lv_obj_t *lbl_hdr = lv_label_create(popup_pairing_win);
  lv_label_set_text(lbl_hdr, "NEUER EMPFAENGER GEFUNDEN");
  lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_hdr, lv_palette_main(LV_PALETTE_ORANGE), 0);

  char info_buf[128];
  snprintf(info_buf, sizeof(info_buf), "Modell: %s\nMAC: %02X:%02X:%02X:%02X:%02X:%02X\nJetzt dauerhaft koppeln?",
           pendingModelName, pendingMac[0], pendingMac[1], pendingMac[2], pendingMac[3], pendingMac[4], pendingMac[5]);

  lv_obj_t *lbl_info = lv_label_create(popup_pairing_win);
  lv_label_set_text(lbl_info, info_buf);
  lv_obj_set_style_text_align(lbl_info, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_12, 0);

  lv_obj_t *btn_row = lv_obj_create(popup_pairing_win);
  lv_obj_set_size(btn_row, 310, 38);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(btn_row, 0, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollable(btn_row, false);

  lv_obj_t *btn_yes = lv_button_create(btn_row);
  lv_obj_set_size(btn_yes, 140, 32);
  lv_obj_set_style_radius(btn_yes, 0, 0);
  lv_obj_set_style_bg_color(btn_yes, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(btn_yes, btn_confirm_pair_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_yes = lv_label_create(btn_yes);
  lv_label_set_text(lbl_yes, "Koppeln");
  lv_obj_center(lbl_yes);

  lv_obj_t *btn_no = lv_button_create(btn_row);
  lv_obj_set_size(btn_no, 140, 32);
  lv_obj_set_style_radius(btn_no, 0, 0);
  lv_obj_set_style_bg_color(btn_no, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_add_event_cb(btn_no, btn_reject_pair_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbl_no = lv_label_create(btn_no);
  lv_label_set_text(lbl_no, "Ignorieren");
  lv_obj_center(lbl_no);

  print_vram_status_pairing("SHOW_PAIRING_POPUP");
}
#pragma endregion

#pragma region INITIALIZATION_AND_PERSISTENCE
/**
 * @brief Initialisiert den Pairing-Manager und laedt bestehende Modellverknuepfungen aus dem LittleFS.
 */
void init_pairing_manager(void) {
  load_paired_models_from_fs();
  register_paired_macs();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] Pairing-Manager initialisiert. Geladene Modelle: %d", pairedModelCount);
  }
  print_vram_status_pairing("INIT_PAIRING_MANAGER");
}

/**
 * @brief Laedt die Liste der gepairten Modelle sowie deren persistierte ModelConfig satzweise aus LittleFS.
 */
void load_paired_models_from_fs(void) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING][LOAD-1] Starte load_paired_models_from_fs()...");
  }

  if (!ensure_paired_models_allocated()) {
    SerialInterface::printError("[PAIRING][LOAD-ERR] Speicher fuer pairedModels konnte nicht alloziert werden!");
    return;
  }

  memset(pairedModels, 0, sizeof(PairedModelInfo) * MAX_PAIRED_MODELS);
  pairedModelCount = 0;

  if (!ensure_littlefs_ready()) {
    return;
  }

  if (!LittleFS.exists(PAIRING_STORAGE_FILE)) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
    SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING][LOAD-2] Keine Speicherdatei ('%s') gefunden. Starte mit leerer Modellliste.",
                                 PAIRING_STORAGE_FILE);
  }
    return;
  }

  File file = LittleFS.open(PAIRING_STORAGE_FILE, "r");
  if (!file) {
    SerialInterface::printError("[PAIRING][LOAD-ERR] Fehler beim Oeffnen von %s zum Lesen!", PAIRING_STORAGE_FILE);
    return;
  }

  size_t expectedBytes = sizeof(PersistedModelEntry) * MAX_PAIRED_MODELS;
  size_t fileSize = file.size();

  if (fileSize != expectedBytes) {
    file.close();
    LittleFS.remove(PAIRING_STORAGE_FILE);
    SerialInterface::printError("[PAIRING][LOAD-WARN] Inkompatible Datei (%u statt %u Bytes) in %s entfernt.",
                                (unsigned int)fileSize, (unsigned int)expectedBytes, PAIRING_STORAGE_FILE);
    return;
  }

  int highestActive = 0;
  size_t totalRead = 0;

  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    PersistedModelEntry entry;
    memset(&entry, 0, sizeof(PersistedModelEntry));
    size_t r = file.read((uint8_t *)&entry, sizeof(PersistedModelEntry));
    totalRead += r;

    if (r == sizeof(PersistedModelEntry) && entry.isPaired) {
      strncpy(pairedModels[i].modelName, entry.modelName, MAX_NAME_LEN - 1);
      pairedModels[i].modelName[MAX_NAME_LEN - 1] = '\0';
      memcpy(pairedModels[i].macAddress, entry.macAddress, 6);
      pairedModels[i].isPaired = true;
      pairedModels[i].sortOrder = entry.sortOrder;
      pairedModels[i].lastTelemetryTime = 0;
      highestActive = i + 1;

      char cfgPath[32];
      build_slot_config_filename(i, cfgPath, sizeof(cfgPath));
      if (LittleFS.exists(cfgPath)) {
        File cfgFile = LittleFS.open(cfgPath, "r");
        if (cfgFile) {
          if (cfgFile.size() == sizeof(ModelConfig)) {
            cfgFile.read((uint8_t *)&pairedModels[i].config, sizeof(ModelConfig));
          }
          cfgFile.close();
        }
      }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
      SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING][LOAD-ITEM] [%d]: '%s' | MAC: %02X:%02X:%02X:%02X:%02X:%02X | Ch=%d",
                                   i, pairedModels[i].modelName,
                                   pairedModels[i].macAddress[0], pairedModels[i].macAddress[1],
                                   pairedModels[i].macAddress[2], pairedModels[i].macAddress[3],
                                   pairedModels[i].macAddress[4], pairedModels[i].macAddress[5],
                                   (int)pairedModels[i].config.channelCount);
  }
    }
  }

  file.close();
  vTaskDelay(1);

  if (totalRead == expectedBytes) {
    pairedModelCount = (int8_t)highestActive;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
    SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING][LOAD-3] Erfolgreich geladen. pairedModelCount=%d (%u Bytes).",
                                 (int)pairedModelCount, (unsigned int)totalRead);
  }
  } else {
    pairedModelCount = 0;
    SerialInterface::printError("[PAIRING][LOAD-ERR] Lesefehler in %s (%u von %u Bytes gelesen).",
                                PAIRING_STORAGE_FILE, (unsigned int)totalRead, (unsigned int)expectedBytes);
  }
}

/**
 * @brief Registriert alle geladenen gepairten Modell-MACs im ESP-NOW Funk-Stack.
 */
void register_paired_macs(void) {
  int registeredCount = 0;
  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired) {
      esp_now_peer_info_t peerInfo;
      memset(&peerInfo, 0, sizeof(peerInfo));
      memcpy(peerInfo.peer_addr, pairedModels[i].macAddress, 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;

      if (!esp_now_is_peer_exist(pairedModels[i].macAddress)) {
        if (esp_now_add_peer(&peerInfo) == ESP_OK) {
          registeredCount++;
        }
      } else {
        registeredCount++;
      }
    }
  }
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] %d gepairte MAC-Adressen als ESP-NOW Peers registriert.", registeredCount);
  }
}

/**
 * @brief Speichert die aktuelle Liste aller gepairten Modelle sowie deren ModelConfig satzweise in LittleFS.
 */
void save_paired_models_to_fs(void) {
  if (!ensure_paired_models_allocated()) {
    SerialInterface::printError("[PAIRING] Fehler: pairedModels nicht alloziert, Speichern abgebrochen!");
    return;
  }

  if (!ensure_littlefs_ready()) {
    return;
  }

  File file = LittleFS.open(PAIRING_STORAGE_FILE, "w");
  if (!file) {
    SerialInterface::printError("[PAIRING] Fehler beim Oeffnen von %s zum Schreiben!", PAIRING_STORAGE_FILE);
    return;
  }

  size_t totalWritten = 0;
  size_t expectedBytes = sizeof(PersistedModelEntry) * MAX_PAIRED_MODELS;

  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    PersistedModelEntry entry;
    memset(&entry, 0, sizeof(PersistedModelEntry));
    char cfgPath[32];
    build_slot_config_filename(i, cfgPath, sizeof(cfgPath));

    if (pairedModels[i].isPaired) {
      strncpy(entry.modelName, pairedModels[i].modelName, MAX_NAME_LEN - 1);
      entry.modelName[MAX_NAME_LEN - 1] = '\0';
      memcpy(entry.macAddress, pairedModels[i].macAddress, 6);
      entry.isPaired = true;
      entry.sortOrder = pairedModels[i].sortOrder;

      if (pairedModels[i].config.channelCount > 0) {
        bool alreadyIdentical = false;
        if (LittleFS.exists(cfgPath)) {
          File checkFile = LittleFS.open(cfgPath, "r");
          if (checkFile) {
            if (checkFile.size() == sizeof(ModelConfig)) {
              ModelConfig diskCfg;
              if (checkFile.read((uint8_t *)&diskCfg, sizeof(ModelConfig)) == sizeof(ModelConfig)) {
                if (memcmp(&diskCfg, &pairedModels[i].config, sizeof(ModelConfig)) == 0) {
                  alreadyIdentical = true;
                }
              }
            }
            checkFile.close();
          }
        }
        if (!alreadyIdentical) {
          File cfgFile = LittleFS.open(cfgPath, "w");
          if (cfgFile) {
            cfgFile.write((const uint8_t *)&pairedModels[i].config, sizeof(ModelConfig));
            cfgFile.close();
          }
        }
      }
    } else if (LittleFS.exists(cfgPath)) {
      LittleFS.remove(cfgPath);
    }

    size_t w = file.write((const uint8_t *)&entry, sizeof(PersistedModelEntry));
    totalWritten += w;
  }

  file.close();
  vTaskDelay(1);

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] %u / %u Bytes in %s gesichert (inkl. Slot-Configs).",
                               (unsigned int)totalWritten, (unsigned int)expectedBytes, PAIRING_STORAGE_FILE);
  }
}
#pragma endregion

#pragma region PAIRING_RUNTIME_AND_MANAGEMENT
void start_pairing_window(void) {
  pairingWindowActive = true;
  pairingWindowStartTime = millis();
  trigger_audio_feedback(AUDIO_EVENT_SWITCH);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
  SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] 30-Sekunden Pairing-Fenster GESTARTET.");
  }
}

void stop_pairing_window(void) {
  pairingWindowActive = false;
  requestShowPopup = false;
}

bool is_pairing_window_active(void) {
  return pairingWindowActive;
}

void update_pairing_manager(void) {
  if (pairingWindowActive) {
    if (millis() - pairingWindowStartTime >= PAIRING_WINDOW_DURATION_MS) {
      pairingWindowActive = false;
      requestShowPopup = false;
      trigger_audio_feedback(AUDIO_EVENT_CLICK);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Pairing)) {
      SerialInterface::printDebug(SerialInterface::Module::Pairing, "[PAIRING] Pairing-Fenster abgelaufen.");
  }
    }
  }

  if (requestShowPopup && !pendingConfirmationOpen) {
    pendingConfirmationOpen = true;
    requestShowPopup = false;
    show_pairing_confirmation_popup();
  }
}

void handle_discovered_peer_during_pairing(const uint8_t *mac, const char *detectedModelName) {
  if (!pairingWindowActive || !mac || !ensure_paired_models_allocated()) return;
  if (pendingConfirmationOpen || requestShowPopup) return;

  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired && memcmp(pairedModels[i].macAddress, mac, 6) == 0) {
      return;
    }
  }

  memcpy(pendingMac, mac, 6);

  if (detectedModelName != NULL && strlen(detectedModelName) > 0) {
    strncpy(pendingModelName, detectedModelName, MAX_NAME_LEN - 1);
    pendingModelName[MAX_NAME_LEN - 1] = '\0';
  } else {
    snprintf(pendingModelName, MAX_NAME_LEN, "RX_%02X%02X", mac[4], mac[5]);
  }

  trigger_audio_feedback(AUDIO_EVENT_WARNING);
  requestShowPopup = true;
}

void update_paired_model_name_if_changed(int modelIndex, const char *newName) {
  if (!ensure_paired_models_allocated()) return;
  if (modelIndex < 0 || modelIndex >= MAX_PAIRED_MODELS || !pairedModels[modelIndex].isPaired) return;
  if (!newName || strlen(newName) == 0) return;

  if (strncmp(pairedModels[modelIndex].modelName, newName, MAX_NAME_LEN) != 0) {
    strncpy(pairedModels[modelIndex].modelName, newName, MAX_NAME_LEN - 1);
    pairedModels[modelIndex].modelName[MAX_NAME_LEN - 1] = '\0';
    save_paired_models_to_fs();
  }
}

void delete_paired_model(int modelIndex) {
  if (!ensure_paired_models_allocated()) return;
  if (modelIndex < 0 || modelIndex >= MAX_PAIRED_MODELS || !pairedModels[modelIndex].isPaired) return;

  if (activeModelIndex == modelIndex) {
    activeModelIndex = -1;
  }

  uint8_t removedOrder = pairedModels[modelIndex].sortOrder;
  if (esp_now_is_peer_exist(pairedModels[modelIndex].macAddress)) {
    esp_now_del_peer(pairedModels[modelIndex].macAddress);
  }

  memset(&pairedModels[modelIndex], 0, sizeof(PairedModelInfo));

  int highestActive = 0;
  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired) {
      if (pairedModels[i].sortOrder > removedOrder) {
        pairedModels[i].sortOrder--;
      }
      highestActive = i + 1;
    }
  }
  pairedModelCount = (int8_t)highestActive;
  save_paired_models_to_fs();
}

void move_paired_model_order(int modelIndex, int direction) {
  if (modelIndex < 0 || modelIndex >= MAX_PAIRED_MODELS || !pairedModels[modelIndex].isPaired) return;

  int currentOrder = (int)pairedModels[modelIndex].sortOrder;
  int targetOrder = currentOrder + direction;
  if (targetOrder < 0) return;

  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired && (int)pairedModels[i].sortOrder == targetOrder) {
      pairedModels[i].sortOrder = (uint8_t)currentOrder;
      pairedModels[modelIndex].sortOrder = (uint8_t)targetOrder;
      save_paired_models_to_fs();
      return;
    }
  }
}
#pragma endregion