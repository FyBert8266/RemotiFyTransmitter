/**
 * @file espnow_comm.cpp
 * @version 1.4.6
 * @brief ESP-NOW Kommunikation, Chunk-basiertes Senden (sendModelConfig), Empfang von MSG_TYPE_CONFIG_DATA
 *        in den globalen Puffer pendingIncomingConfig, Modellnamen-Synchronisation fuer Top-Bar,
 *        Dashboard-Neuaufbau bei Modellwechsel, Offline-Erkennung (>2s), Top-Bar und Debugging.
 * @Version_info v1.4.6: Verarbeitung von pendingIncomingConfig (handle_received_config_logic) aus
 *               onDataReceive() entkoppelt (process_pending_received_config() im Haupt-Loop via
 *               update_async_model_config_sender()); alle bestehenden Zeilen und Debug-BlÃ¶cke erhalten.
 *               v1.4.5: Linker-Fehler ('undefined reference to last_active_idx' und 'build_dashboard_page()')
 *               durch entkoppelten weak-Hook (on_active_model_changed_ui_hook) via lv_async_call behoben.
 *               v1.4.4: Top-Bar verwendet bevorzugt pairedModels[modelIdx].config.modelName (mit Fallback auf
 *               pairedModels[modelIdx].modelName); bei Modellwechsel (multi_model_btn_cb) wird bei aktivem
 *               Dashboard-Tab (last_active_idx == 0) asynchron build_dashboard_page() ausgeloesst.
 */


#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "config_types.h"
#include "espnow_comm.h"
#include "SerialInterface.h"
#include "audio_module.h"
#include "pairing_manager.h"
#include "model_page.h"

// Externe Deklaration der Verarbeitungsfunktion fuer empfangene Konfigurationen (model_page.cpp)
extern void handle_received_config_logic(void);

/**
 * @brief Optionaler UI-Hook bei Aenderung des aktiven Modells (kann in Haupt-Sketch oder Dashboard ueberschrieben werden).
 * @param newModelIndex Neuer aktiver Modell-Index (-1 = kein Modell aktiv).
 */
__attribute__((weak)) void on_active_model_changed_ui_hook(int newModelIndex) {
  (void)newModelIndex;
}

#pragma region GLOBALS_AND_STATICS
/** @brief Dynamisch alloziertes Array der gepairten Modelle (PSRAM bevorzugt, Fallback interner Heap). */
PairedModelInfo *pairedModels = NULL;

/**
 * @brief Alloziert das pairedModels-Array dynamisch, falls noch nicht geschehen.
 * @return true bei Erfolg, sonst false.
 */
bool ensure_paired_models_allocated(void) {
  if (pairedModels != NULL) return true;

  size_t totalBytes = sizeof(PairedModelInfo) * MAX_PAIRED_MODELS;
  pairedModels = (PairedModelInfo *)heap_caps_malloc(totalBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (pairedModels == NULL) {
    pairedModels = (PairedModelInfo *)heap_caps_malloc(totalBytes, MALLOC_CAP_8BIT);
  }
  if (pairedModels != NULL) {
    memset(pairedModels, 0, totalBytes);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
    SerialInterface::printDebug(SerialInterface::Module::Memory, "[MEM] pairedModels dynamisch alloziert: %u Bytes (%d Slots)\n",
                  (unsigned int)totalBytes, MAX_PAIRED_MODELS);
  }
    return true;
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ERROR] Konnte pairedModels (%u Bytes) nicht allozieren!\n",
                (unsigned int)totalBytes);
  }
  return false;
}

/** @brief Aktuelle Anzahl der erfolgreich gepairten Modelle im System. */
int8_t pairedModelCount = 0;

/** @brief Index des aktuell aktiven und verbundenen Modells im pairedModels-Array (-1 = kein Modell aktiv). */
int8_t activeModelIndex = -1;

/** @brief Kompatibilitaets-Bruecke: MAC-Adresse des aktiven Modells fuer Core-Tasks. */
uint8_t receiverMAC[6] = {0};

/** @brief Zwischenspeicher fuer die zuletzt empfangenen Telemetriedaten des aktiven Modells. */
TelemetryMessage incomingTelemetry;

/** @brief Flag: Gibt an, ob gueltige Telemetriedaten fuer das aktive Modell vorliegen. */
bool hasTelemetry = false;

/** @brief Zeitstempel (Millis) des letzten erfolgreichen Telemetrie-Empfangs vom aktiven Modell. */
unsigned long lastTelemetryTime = 0;

/** @brief Array von Zeigern auf die LVGL-Schaltflaechen der Top-Bar. */
lv_obj_t *model_btn_array[MAX_VISIBLE_TOP_BAR];

// Globale / statische Puffer fuer den Empfang von Konfigurations-Chunks (MSG_TYPE_CONFIG_DATA)
ModelConfig pendingIncomingConfig;
static ModelConfig incomingConfigBuffer;
static uint8_t expectedSenderChunks = 0;
static uint32_t receivedSenderChunksMask = 0;
static uint16_t activeSenderTransferId = 0;
static uint8_t senderTransferSourceMAC[6] = {0};
static bool senderConfigTransferActive = false;
static unsigned long lastSenderChunkMs = 0;
static const unsigned long senderChunkTimeoutMs = 1000;
static volatile bool pendingConfigReadyToProcess = false;

// Globale Variablen fuer den asynchronen Config-Versand
static bool isSendingConfigAsync = false;
static int asyncBytesSent = 0;
static uint8_t asyncChunkIdx = 0;
static uint8_t asyncTotalChunks = 0;
static uint16_t asyncTransferId = 0;
static unsigned long lastAsyncChunkTime = 0;
static ModelConfig asyncConfigCopy;
static uint8_t asyncTargetMac[6];
static uint16_t nextConfigTransferId = 0;

static portMUX_TYPE txDiagnosticsMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t txCallbackSuccessCount = 0;
static uint32_t txCallbackFailureCount = 0;
static uint32_t txQueueErrorCount = 0;

static void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status) {
  (void)macAddr;
  portENTER_CRITICAL(&txDiagnosticsMux);
  if (status == ESP_NOW_SEND_SUCCESS) {
    txCallbackSuccessCount++;
  } else {
    txCallbackFailureCount++;
  }
  portEXIT_CRITICAL(&txDiagnosticsMux);
}

/**
 * @brief Liefert den aktuell gueltigen Anzeigenamen eines gepairten Modells fuer Top-Bar und UI.
 * @param modelIdx Index im pairedModels-Array.
 * @return Zeiger auf den Modellnamen.
 */
static const char *get_display_model_name(int modelIdx) {
  if (pairedModels != NULL && modelIdx >= 0 && modelIdx < MAX_PAIRED_MODELS) {
    if (strlen(pairedModels[modelIdx].config.modelName) > 0) {
      return pairedModels[modelIdx].config.modelName;
    }
    if (strlen(pairedModels[modelIdx].modelName) > 0) {
      return pairedModels[modelIdx].modelName;
    }
  }
  return "Unbekannt";
}

/**
 * @brief Asynchroner LVGL-Callback zum Benachrichtigen der UI bei Modellwechsel.
 * @param user_data Aktiver Modell-Index als intptr_t.
 */
static void notify_model_switch_async_cb(void *user_data) {
  int newIdx = (int)(intptr_t)user_data;
  if (newIdx != activeModelIndex) return;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[DEBUG] Modellwechsel UI-Hook ausgeloesst (newModelIndex=%d).\n", newIdx);
  }
  on_active_model_changed_ui_hook(newIdx);
}

/**
 * @brief Verarbeitet eine komplett empfangene Konfiguration sicher ausserhalb von onDataReceive().
 */
void process_pending_received_config(void) {
  if (!pendingConfigReadyToProcess) return;
  pendingConfigReadyToProcess = false;
  handle_received_config_logic();
}

#pragma endregion

#pragma region INTERNAL_VRAM_DIAGNOSE
/**
 * @brief Gibt den aktuellen LVGL-VRAM-Status im seriellen Monitor aus (zur Speicherdiagnose).
 * @param tag Bezeichner fuer den Aufrufkontext.
 */
static void print_vram_status_comm(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Groesster freier Block: %u Bytes\n",
                tag, (int)mon.used_pct, (unsigned int)mon.free_biggest_size);
  }
}
#pragma endregion

#pragma region ESP_NOW_CALLBACKS
/**
 * @brief Zentraler Callback beim Empfang von ESP-NOW Daten mit strukturiertem switch-case Dispatcher und Debug-Ausgaben.
 * @param macAddr MAC-Adresse des Senders.
 * @param incomingData Zeiger auf die empfangenen Rohdaten.
 * @param len Laenge der Daten in Bytes.
 */
static void onDataReceive(const uint8_t *macAddr, const uint8_t *incomingData, int len) {
  if (len < 1 || !macAddr || !incomingData) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
    SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] onDataReceive abgebrochen: len=%d, macAddr oder Daten ungueltig.", len);
  }
    return;
  }
  if (!ensure_paired_models_allocated()) return;

  uint8_t packetType = incomingData[0];

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Paket empfangen! Absender-MAC: %02X:%02X:%02X:%02X:%02X:%02X | Typ: 0x%02X | Laenge: %d",
                               macAddr[0], macAddr[1], macAddr[2],
                               macAddr[3], macAddr[4], macAddr[5],
                               packetType, len);
  }

  int senderIndex = -1;
  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired && memcmp(pairedModels[i].macAddress, macAddr, 6) == 0) {
      senderIndex = i;
      break;
    }
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] RX Paket: Typ=0x%02X, Laenge=%d, Absender-MAC=%02X:%02X:%02X:%02X:%02X:%02X (Index: %d)",
                               packetType, len,
                               macAddr[0], macAddr[1], macAddr[2],
                               macAddr[3], macAddr[4], macAddr[5],
                               senderIndex);
  }

  switch (packetType) {
    case MSG_TYPE_PAIRING: {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
      SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Verzweigung: MSG_TYPE_PAIRING (0x03)");
  }
      if (len != (int)sizeof(PairingMessage)) break;
      PairingMessage pMsg;
      memcpy(&pMsg, incomingData, sizeof(pMsg));
      if (pMsg.protocolVersion != PAIRING_PROTOCOL_VERSION) break;
      pMsg.modelName[MAX_NAME_LEN - 1] = '\0';

      if (pMsg.action == PAIRING_ACTION_ACK) {
        if (senderIndex >= 0 && memcmp(pMsg.receiverMAC, macAddr, 6) == 0) {
          handle_pairing_ack(macAddr, pMsg.nonce);
        }
        break;
      }

      if (pMsg.action != PAIRING_ACTION_DISCOVER ||
          !is_pairing_window_active() ||
          senderIndex >= 0 ||
          pMsg.nonce == 0 ||
          memcmp(pMsg.receiverMAC, macAddr, 6) != 0) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
        SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Pairing ignoriert (Fenster aktiv: %s, Absender bekannt Index: %d)",
                                     is_pairing_window_active() ? "JA" : "NEIN", senderIndex);
  }
        break;
      }
      if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
        SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Pairing-Paket gueltig. Name: '%s'", pMsg.modelName);
      }
      handle_discovered_peer_during_pairing(macAddr, pMsg.modelName, pMsg.nonce);
      break;
    }

    case MSG_TYPE_RECEIVER_STATUS: {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
      SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Verzweigung: MSG_TYPE_RECEIVER_STATUS (0x04)");
  }
      if (len == (int)sizeof(ReceiverStatusMessage)) {
        ReceiverStatusMessage *statusMsg = (ReceiverStatusMessage *)incomingData;
        if (senderIndex >= 0 && senderIndex < MAX_PAIRED_MODELS && pairedModels[senderIndex].isPaired) {
          pairedModels[senderIndex].isOccupiedByOther = statusMsg->isOccupied;
          strncpy(pairedModels[senderIndex].occupantTransmitterName, statusMsg->activeTransmitterName, TRANSMITTER_NAME_LEN - 1);
          pairedModels[senderIndex].occupantTransmitterName[TRANSMITTER_NAME_LEN - 1] = '\0';

          unsigned long oldTime = pairedModels[senderIndex].lastTelemetryTime;
          pairedModels[senderIndex].lastTelemetryTime = millis();

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
          SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Status aktualisiert fuer Modell '%s' (Index %d): Occupied=%s, lastTime: %lu -> %lu",
                                       get_display_model_name(senderIndex), senderIndex,
                                       statusMsg->isOccupied ? "JA" : "NEIN",
                                       oldTime, pairedModels[senderIndex].lastTelemetryTime);
  }
        } else {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
          SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Status empfangen, aber Absender-Index ungueltig (%d)", senderIndex);
  }
        }
      } else {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
        SerialInterface::printError("[ESP-NOW DEBUG] Fehler: Laenge von ReceiverStatusMessage stimmt nicht! Erwartet: %u, Erhalten: %d",
                                    (unsigned int)sizeof(ReceiverStatusMessage), len);
  }
      }
      break;
    }

    case MSG_TYPE_TELEMETRY: {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Telemetry)) {
      SerialInterface::printDebug(SerialInterface::Module::Telemetry, "[TELEMETRY] Paket empfangen.");
  }
      if (len == (int)sizeof(TelemetryMessage)) {
        TelemetryMessage tempTel;
        memcpy(&tempTel, incomingData, sizeof(TelemetryMessage));


        if (senderIndex >= 0) {
          pairedModels[senderIndex].telemetry = tempTel;
          pairedModels[senderIndex].hasTelemetry = true;
          pairedModels[senderIndex].lastTelemetryTime = millis();

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Telemetry)) {
          SerialInterface::printDebug(SerialInterface::Module::Telemetry, "[TELEMETRY] Modell '%s' (Index %d), RSSI: %d dBm",
                                       get_display_model_name(senderIndex), senderIndex, tempTel.rssi);
  }

          if (activeModelIndex == senderIndex) {
            incomingTelemetry = tempTel;
            hasTelemetry = true;
            lastTelemetryTime = millis();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Telemetry)) {
            SerialInterface::printDebug(SerialInterface::Module::Telemetry, "[TELEMETRY] Daten an aktives Modell uebergeben.");
  }
          }
        } else {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Telemetry)) {
          SerialInterface::printDebug(SerialInterface::Module::Telemetry, "[TELEMETRY] Daten von unbekanntem Peer empfangen.");
  }
        }
      } else {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Telemetry)) {
        SerialInterface::printError("[TELEMETRY][ERROR] Paketlaenge falsch (erwartet %u, erhalten %d)",
                                    (unsigned int)sizeof(TelemetryMessage), len);
  }
      }
      break;
    }

    case MSG_TYPE_CONFIG_DATA: {
      if (senderIndex < 0 || len != (int)sizeof(ESPNowTransferMessage)) break;
      ESPNowTransferMessage msg;
      memcpy(&msg, incomingData, sizeof(msg));

      const uint8_t expectedChunks = (uint8_t)((sizeof(ModelConfig) + 239U) / 240U);
      if (msg.totalChunks != expectedChunks || msg.chunkIndex >= expectedChunks) break;
      const size_t offset = (size_t)msg.chunkIndex * 240U;
      const size_t expectedLength = min((size_t)240U, sizeof(ModelConfig) - offset);
      if (msg.dataLength != expectedLength) break;

      const unsigned long now = millis();
      if (senderConfigTransferActive && now - lastSenderChunkMs > senderChunkTimeoutMs) {
        senderConfigTransferActive = false;
        receivedSenderChunksMask = 0;
      }

      if (!senderConfigTransferActive ||
          msg.transferId != activeSenderTransferId ||
          memcmp(senderTransferSourceMAC, macAddr, 6) != 0) {
        if (msg.chunkIndex != 0) break;
        memset(&incomingConfigBuffer, 0, sizeof(incomingConfigBuffer));
        activeSenderTransferId = msg.transferId;
        memcpy(senderTransferSourceMAC, macAddr, 6);
        expectedSenderChunks = expectedChunks;
        receivedSenderChunksMask = 0;
        senderConfigTransferActive = true;
      }

      const uint32_t chunkBit = 1UL << msg.chunkIndex;
      if ((receivedSenderChunksMask & chunkBit) != 0) break;
      memcpy(((uint8_t *)&incomingConfigBuffer) + offset, msg.payload, expectedLength);
      receivedSenderChunksMask |= chunkBit;
      lastSenderChunkMs = now;
      const uint32_t expectedMask = (1UL << expectedSenderChunks) - 1UL;

      if (receivedSenderChunksMask == expectedMask) {
        pendingIncomingConfig = incomingConfigBuffer;
        senderConfigTransferActive = false;
        receivedSenderChunksMask = 0;
        expectedSenderChunks = 0;

        if (pendingIncomingConfig.channelCount == 0 ||
            pendingIncomingConfig.channelCount > MAX_CHANNELS ||
            memchr(pendingIncomingConfig.modelName, '\0', MAX_NAME_LEN) == NULL) {
          break;
        }

        pairedModels[senderIndex].lastTelemetryTime = now;
        if (pendingIncomingConfig.modelName[0] != '\0') {
          strncpy(pairedModels[senderIndex].modelName, pendingIncomingConfig.modelName, MAX_NAME_LEN - 1);
          pairedModels[senderIndex].modelName[MAX_NAME_LEN - 1] = '\0';
        }

        if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
          SerialInterface::printDebug(SerialInterface::Module::EspNow,
                                      "[ESP-NOW] Modellkonfiguration '%s' (%u Kanaele) vollstaendig empfangen.",
                                      pendingIncomingConfig.modelName,
                                      (unsigned int)pendingIncomingConfig.channelCount);
        }
        pendingConfigReadyToProcess = true;
      }
      break;
    }

    case MSG_TYPE_CONFIG_REQ: {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
      SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Verzweigung: CONFIG_REQ (0x%02X)", packetType);
  }
      if (senderIndex >= 0 && len == (int)sizeof(ESPNowTransferMessage)) {
        ESPNowTransferMessage request;
        memcpy(&request, incomingData, sizeof(request));
        if (request.chunkIndex != 0 || request.totalChunks != 0 ||
            request.dataLength != 0 || request.transferId != 0) break;
        pairedModels[senderIndex].lastTelemetryTime = millis();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
        SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] lastTelemetryTime aktualisiert durch Config-Paket fuer Index %d", senderIndex);
  }
      }
      break;
    }

    default: {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
      SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] Verzweigung: DEFAULT (Unbekannter Typ 0x%02X)", packetType);
  }
      if (packetType != 0x00 && senderIndex >= 0) {
        pairedModels[senderIndex].lastTelemetryTime = millis();
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
        SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW DEBUG] lastTelemetryTime aktualisiert durch Default-Paket fuer Index %d", senderIndex);
  }
      }
      break;
    }
  }
}
#pragma endregion

#pragma region INITIALIZATION_AND_CONFIG_REQUEST
/**
 * @brief Initialisiert das ESP-NOW Kommunikationsmodul und registriert den Empfangs-Callback.
 */
void init_espnow_comm(void) {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
    SerialInterface::printError("[ESP-NOW] Fehler bei der Initialisierung von ESP-NOW!");
  }
    return;
  }

  ensure_paired_models_allocated();
  // Pairing-Manager initialisieren und gespeicherte Modelle aus LittleFS laden
  init_pairing_manager();

  esp_now_register_recv_cb(onDataReceive);
  esp_now_register_send_cb(onDataSent);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW] Kommunikationsmodul erfolgreich initialisiert.");
  }
}

void record_live_control_send_result(esp_err_t result) {
  if (result == ESP_OK) return;
  portENTER_CRITICAL(&txDiagnosticsMux);
  txQueueErrorCount++;
  portEXIT_CRITICAL(&txDiagnosticsMux);
}

void update_espnow_diagnostics(void) {
  static unsigned long reportWindowStartMs = 0;
  const unsigned long now = millis();
  if (reportWindowStartMs == 0) {
    reportWindowStartMs = now;
    return;
  }
  if (now - reportWindowStartMs < 10000UL) return;
  reportWindowStartMs = now;

  uint32_t successes;
  uint32_t failures;
  uint32_t queueErrors;
  portENTER_CRITICAL(&txDiagnosticsMux);
  successes = txCallbackSuccessCount;
  failures = txCallbackFailureCount;
  queueErrors = txQueueErrorCount;
  txCallbackSuccessCount = 0;
  txCallbackFailureCount = 0;
  txQueueErrorCount = 0;
  portEXIT_CRITICAL(&txDiagnosticsMux);

  if (!SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) return;
  const uint32_t completed = successes + failures;
  const float successPercent = (completed > 0)
                                 ? (100.0f * (float)successes / (float)completed)
                                 : 0.0f;
  SerialInterface::printDebug(SerialInterface::Module::EspNow,
                              "[ESP-NOW TX 10s] MAC-Rueckmeldung: %lu OK, %lu Fehler (%.1f%% OK), Queue-Fehler: %lu",
                              (unsigned long)successes, (unsigned long)failures,
                              successPercent, (unsigned long)queueErrors);
}

/**
 * @brief Sendet eine Anfrage an einen Peer, um dessen aktuelle Modellkonfiguration abzurufen.
 * @param peer_mac 6-Byte MAC-Adresse des Ziel-Empfaengers.
 */
void request_model_config_from_peer(const uint8_t *peer_mac) {
  if (!peer_mac) return;

  ESPNowTransferMessage reqMsg;
  memset(&reqMsg, 0, sizeof(reqMsg));
  reqMsg.packetType = MSG_TYPE_CONFIG_REQ;

  esp_err_t result = esp_now_send(peer_mac, (uint8_t *)&reqMsg, sizeof(reqMsg));
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  if (result == ESP_OK) {
    SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW] Konfigurationsanfrage erfolgreich an Peer gesendet.");
  } else {
    SerialInterface::printError("[ESP-NOW] Fehler beim Senden der Konfigurationsanfrage: %d", result);
  }
  }
}

/**
 * @brief Sendet eine komplette Modellkonfiguration in Chunks per ESP-NOW an einen spezifischen Peer.
 * @param config Referenz auf die zu sendende ModelConfig.
 * @param peer_mac 6-Byte MAC-Adresse des Empfaengers.
 */
void sendModelConfig(const ModelConfig &config, const uint8_t *peer_mac) {
  if (!peer_mac) return;
  if (peer_mac[0] == 0 && peer_mac[1] == 0 && peer_mac[2] == 0 &&
      peer_mac[3] == 0 && peer_mac[4] == 0 && peer_mac[5] == 0) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
    SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW ERROR] Abbruch: Keine Ziel-MAC (receiverMAC) zugewiesen! Bitte Modell in Top-Bar waehlen.");
  }
    return;
  }

  const uint8_t *rawData = (const uint8_t *)&config;
  int totalSize = sizeof(ModelConfig);
  int bytesSent = 0;
  uint8_t chunkIdx = 0;
  uint8_t totalChunks = (totalSize + 239) / 240;
  uint16_t transferId = ++nextConfigTransferId;
  if (transferId == 0) transferId = ++nextConfigTransferId;

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW] Sende Modellkonfiguration an Peer (%d Bytes in %d Chunks)...", totalSize, totalChunks);
  }

  while (bytesSent < totalSize) {
    ESPNowTransferMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.packetType = MSG_TYPE_CONFIG_DATA;
    msg.chunkIndex = chunkIdx;
    msg.totalChunks = totalChunks;
    msg.transferId = transferId;
    int bytesToCopy = min(240, totalSize - bytesSent);
    msg.dataLength = bytesToCopy;
    memcpy(msg.payload, rawData + bytesSent, bytesToCopy);

    esp_now_send(peer_mac, (uint8_t *)&msg, sizeof(msg));
    bytesSent += bytesToCopy;
    chunkIdx++;

    delay(20);
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW] Modellkonfiguration erfolgreich an Peer gesendet.");
  }
}

/**
 * @brief Startet den asynchronen, nicht-blockierenden Konfigurationsversand.
 */
void trigger_async_model_config_send(const ModelConfig &config, const uint8_t *peer_mac) {
  if (!peer_mac) return;
  if (peer_mac[0] == 0 && peer_mac[1] == 0 && peer_mac[2] == 0 &&
      peer_mac[3] == 0 && peer_mac[4] == 0 && peer_mac[5] == 0) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
    SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW ERROR] Abbruch: Keine Ziel-MAC zugewiesen!");
  }
    return;
  }

  asyncConfigCopy = config;
  memcpy(asyncTargetMac, peer_mac, 6);
  asyncBytesSent = 0;
  asyncChunkIdx = 0;
  asyncTotalChunks = (sizeof(ModelConfig) + 239) / 240;
  asyncTransferId = ++nextConfigTransferId;
  if (asyncTransferId == 0) asyncTransferId = ++nextConfigTransferId;
  isSendingConfigAsync = true;
  lastAsyncChunkTime = millis();

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
  SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW] Asynchroner Config-Versand gestartet...");
  }
}

/**
 * @brief Zyklische Aktualisierungsfunktion fuer den asynchronen Versand (muss im loop() aufgerufen werden).
 */
void update_async_model_config_sender(void) {
  process_pending_received_config();

  if (!isSendingConfigAsync) return;

  unsigned long now = millis();
  if (now - lastAsyncChunkTime >= 20) {
    lastAsyncChunkTime = now;

    int totalSize = sizeof(ModelConfig);
    if (asyncBytesSent < totalSize) {
      const uint8_t *rawData = (const uint8_t *)&asyncConfigCopy;

      ESPNowTransferMessage msg;
      memset(&msg, 0, sizeof(msg));
      msg.packetType = MSG_TYPE_CONFIG_DATA;
      msg.chunkIndex = asyncChunkIdx;
      msg.totalChunks = asyncTotalChunks;
      msg.transferId = asyncTransferId;

      int bytesToCopy = min(240, totalSize - asyncBytesSent);
      msg.dataLength = bytesToCopy;
      memcpy(msg.payload, rawData + asyncBytesSent, bytesToCopy);

      esp_now_send(asyncTargetMac, (uint8_t *)&msg, sizeof(msg));

      asyncBytesSent += bytesToCopy;
      asyncChunkIdx++;
    } else {
      isSendingConfigAsync = false;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::EspNow)) {
      SerialInterface::printDebug(SerialInterface::Module::EspNow, "[ESP-NOW] Asynchroner Config-Versand erfolgreich abgeschlossen.");
  }
    }
  }
}
#pragma endregion

#pragma region TOP_BAR_UI_MANAGEMENT
/**
 * @brief Event-Callback bei Klick auf einen Top-Bar Button (Modell-Wechsel oder Verbindungsaufbau).
 * @param e Zeiger auf das ausgeloeste LVGL-Ereignis.
 */
void multi_model_btn_cb(lv_event_t *e) {
  int modelIdx = (int)(intptr_t)lv_event_get_user_data(e);
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] Button geklickt fuer Modell-Index: %d", modelIdx);
  }

  if (modelIdx < 0 || modelIdx >= MAX_PAIRED_MODELS || !pairedModels[modelIdx].isPaired) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] Ungueltiger Modell-Index oder nicht gepairt.");
  }
    return;
  }

  bool isOffline = (millis() - pairedModels[modelIdx].lastTelemetryTime > 2000);
  if (isOffline) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printError("[TOP-BAR DEBUG] Kann nicht verbinden: Empfaenger ist OFFLINE!");
  }
    trigger_audio_feedback(AUDIO_EVENT_WARNING);
    return;
  }

  if (activeModelIndex == modelIdx) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] Aktives Modell %d abgemeldet (Trennung).", modelIdx);
  }
    activeModelIndex = -1;
    hasTelemetry = false;
    memset(&incomingTelemetry, 0, sizeof(TelemetryMessage));
    memset(receiverMAC, 0, 6);
    on_model_page_active_model_changed(-1);
    trigger_audio_feedback(AUDIO_EVENT_CLICK);
    lv_async_call(notify_model_switch_async_cb, (void *)(intptr_t)(-1));
  } else {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] Modell %d als aktiv ausgewaehlt ('%s').",
                                 modelIdx, get_display_model_name(modelIdx));
  }
    activeModelIndex = modelIdx;
    memcpy(receiverMAC, pairedModels[modelIdx].macAddress, 6);
    if (pairedModels[modelIdx].hasTelemetry) {
      incomingTelemetry = pairedModels[modelIdx].telemetry;
      hasTelemetry = true;
      lastTelemetryTime = pairedModels[modelIdx].lastTelemetryTime;
    } else {
      hasTelemetry = false;
    }
    on_model_page_active_model_changed(modelIdx);
    trigger_audio_feedback(AUDIO_EVENT_MODEL_SWITCH);
    lv_async_call(notify_model_switch_async_cb, (void *)(intptr_t)modelIdx);
  }
}

/**
 * @brief Aktualisiert die Top-Bar dynamisch mit Sitzungs-Filter und Offline-Erkennung.
 * @param parent_cont Zeiger auf den uebergeordneten Container der Top-Bar.
 */
void update_top_bar_ui(lv_obj_t *parent_cont) {
  if (!parent_cont) return;

  static bool initialized = false;
  if (!initialized) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
    SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] Initialisiere Top-Bar Buttons...");
  }
    lv_obj_clean(parent_cont);
    lv_obj_set_flex_flow(parent_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(parent_cont, 0, 0);
    lv_obj_set_style_pad_column(parent_cont, 1, 0);

    for (int i = 0; i < MAX_VISIBLE_TOP_BAR; i++) {
      model_btn_array[i] = lv_button_create(parent_cont);
      lv_obj_set_flex_grow(model_btn_array[i], 1);
      lv_obj_set_height(model_btn_array[i], 24);
      lv_obj_set_style_pad_all(model_btn_array[i], 0, 0);
      lv_obj_set_style_radius(model_btn_array[i], 0, 0);

      lv_obj_t *lbl = lv_label_create(model_btn_array[i]);
      lv_obj_center(lbl);
      lv_obj_add_event_cb(model_btn_array[i], multi_model_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)(-1));
    }
    initialized = true;
  }

  update_pairing_manager();

  int activeIndices[MAX_PAIRED_MODELS];
  int activeCount = 0;
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::UI)) {
  SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] === Telemetrie-Zeitstempel Status (Count: %d) ===", pairedModelCount);
  unsigned long current_ms = millis();
  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (pairedModels[i].isPaired) {
      unsigned long age = (pairedModels[i].lastTelemetryTime > 0) ? (current_ms - pairedModels[i].lastTelemetryTime) : 0;
      SerialInterface::printDebug(SerialInterface::Module::UI, "[TOP-BAR DEBUG] [%d] '%s' | lastTime: %lu (Alter: %lu ms) | isOccupied: %s",
                                   i,
                                   get_display_model_name(i),
                                   pairedModels[i].lastTelemetryTime,
                                   age,
                                   pairedModels[i].isOccupiedByOther ? "JA" : "NEIN");
    }
  }
  }

  for (int i = 0; i < MAX_PAIRED_MODELS; i++) {
    if (!pairedModels[i].isPaired) continue;

    if (pairedModels[i].lastTelemetryTime > 0) {
      activeIndices[activeCount++] = i;
    }
  }

  for (int i = 0; i < activeCount - 1; i++) {
    for (int j = i + 1; j < activeCount; j++) {
      if (pairedModels[activeIndices[j]].sortOrder < pairedModels[activeIndices[i]].sortOrder) {
        int temp = activeIndices[i];
        activeIndices[i] = activeIndices[j];
        activeIndices[j] = temp;
      }
    }
  }

  if (activeCount == 0) {
    lv_obj_set_hidden(model_btn_array[0], false);
    lv_obj_remove_event_cb(model_btn_array[0], multi_model_btn_cb);

    lv_obj_t *lbl = lv_obj_get_child(model_btn_array[0], 0);
    if (lbl) lv_label_set_text(lbl, "Kein Signal / Bereit");
    lv_obj_set_style_bg_color(model_btn_array[0], lv_palette_darken(LV_PALETTE_GREY, 2), 0);

    for (int slot = 1; slot < MAX_VISIBLE_TOP_BAR; slot++) {
      lv_obj_set_hidden(model_btn_array[slot], true);
    }
  } else {
    for (int slot = 0; slot < MAX_VISIBLE_TOP_BAR; slot++) {
      if (slot < activeCount) {
        int modelIdx = activeIndices[slot];
        lv_obj_set_hidden(model_btn_array[slot], false);

        lv_obj_remove_event_cb(model_btn_array[slot], multi_model_btn_cb);
        lv_obj_add_event_cb(model_btn_array[slot], multi_model_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)modelIdx);

        lv_obj_t *lbl = lv_obj_get_child(model_btn_array[slot], 0);
        char buf[64];
        const char *dispName = get_display_model_name(modelIdx);

        bool isOffline = (millis() - pairedModels[modelIdx].lastTelemetryTime > 2000);

        if (isOffline) {
          snprintf(buf, sizeof(buf), "%s (OFFLINE)", dispName);
          lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_RED), 0);
        } else if (modelIdx == activeModelIndex) {
          int8_t rssi = pairedModels[modelIdx].telemetry.rssi;
          if (rssi > -127 && rssi <= 0) {
            snprintf(buf, sizeof(buf), "%s (%d dBm)", dispName, rssi);
            if (rssi >= -55) {
              lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_GREEN), 0);
            } else if (rssi >= -67) {
              lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_LIME), 0);
            } else if (rssi >= -75) {
              lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_ORANGE), 0);
            } else {
              lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_RED), 0);
            }
          } else {
            snprintf(buf, sizeof(buf), "%s (RSSI --)", dispName);
            lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_GREY), 0);
          }
        } else if (pairedModels[modelIdx].isOccupiedByOther) {
          snprintf(buf, sizeof(buf), "%s (%s)", dispName, pairedModels[modelIdx].occupantTransmitterName);
          lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_ORANGE), 0);
        } else {
          snprintf(buf, sizeof(buf), "%s (IDLE)", dispName);
          lv_obj_set_style_bg_color(model_btn_array[slot], lv_palette_main(LV_PALETTE_GREY), 0);
        }
        if (lbl) lv_label_set_text(lbl, buf);
      } else {
        lv_obj_set_hidden(model_btn_array[slot], true);
      }
    }
  }
  print_vram_status_comm("UPDATE_TOP_BAR_UI");
}
#pragma endregion