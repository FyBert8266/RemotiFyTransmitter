/**
 * @file espnow_comm.h
 * @version 1.1.2
 * @brief Header-Datei fuer ESP-NOW Kommunikation, Multi-Link, Peer-Verwaltung und Telemetrie (v1.1.2).
 * @Version_info v1.1.2: Prototyp process_pending_received_config(void) ergaenzt und doppelte
 *               Deklaration von sendModelConfig bereinigt.
 *               v1.1.1: Ergaenzung von Kompatibilitaets-Wrappern (init_espnow, sendModelConfig) fuer Bestands-Sketches.
 */

#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <Arduino.h>
#include <esp_now.h>
#include <lvgl.h>
#include "config_types.h"

#pragma region DEFINES_AND_CONSTANTS
#ifndef MAX_PAIRED_MODELS
#define MAX_PAIRED_MODELS 5
#endif

// #ifndef TRANSMITTER_NAME_LEN
// #define TRANSMITTER_NAME_LEN 16
// #endif

#define AUDIO_EVENT_MODEL_SWITCH 0x01
#define AUDIO_EVENT_SWITCH       0x02
#pragma endregion

#pragma region DATA_STRUCTURES
// /**
//  * @brief Struktur fuer den Empfaenger-Status (Multi-Sender Lock).
//  */
// struct __attribute__((packed)) ReceiverStatusMessage {
//   uint8_t packetType;
//   bool isOccupied;
//   char activeTransmitterName[TRANSMITTER_NAME_LEN];
// };

// /**
//  * @brief Struktur fuer gepaarte Modelle.
//  */
// struct PairedModelInfo {
//   char modelName[MAX_NAME_LEN];
//   uint8_t macAddress[6];
//   bool isOccupiedByOther;
//   char occupantTransmitterName[TRANSMITTER_NAME_LEN];
// };
#pragma endregion

#pragma region EXTERNAL_VARIABLES
extern uint8_t receiverMAC[];
extern ModelConfig incomingConfig;
extern ModelConfig pendingIncomingConfig;
extern TelemetryMessage incomingTelemetry;
extern bool hasTelemetry;
extern unsigned long lastTelemetryTime;

extern PairedModelInfo *pairedModels;
extern int8_t pairedModelCount;
extern int8_t activeModelIndex;
extern LiveControlMessage liveMsg;
#pragma endregion

#pragma region FUNCTION_PROTOTYPES
/**
 * @brief Initialisiert WLAN im Stations-Modus, startet ESP-NOW und registriert Peers.
 */
void init_espnow_comm(void);

/**
 * @brief Inline-Kompatibilitaets-Wrapper fuer init_espnow aus dem Haupt-Sketch.
 */
inline void init_espnow() {
  init_espnow_comm();
}

/**
 * @brief Wechselt das aktive Modell auf den angegebenen Index.
 */
void select_active_model(int8_t index);

/**
 * @brief Sendet zyklische Live-Steuerdaten an das aktive Modell.
 */
void send_live_control_data(void);

/**
 * @brief Sendet eine vollstaendige Modellkonfiguration in Chunks an den Empfaenger.
 */
// void send_config_to_receiver(void);
void sendModelConfig(const ModelConfig &config, const uint8_t *peer_mac);

/**
 * @brief Aktualisiert die obere Statusleiste mit den Modell-Buttons und RSSI.
 */
void update_top_bar_ui(lv_obj_t *parent_cont);

/**
 * @brief Triggert akustisches Feedback.
 */
void trigger_audio_feedback(uint8_t event_id);

void request_model_config_from_peer(const uint8_t *peer_mac);

void process_pending_received_config(void);

void update_async_model_config_sender(void);

void trigger_async_model_config_send(const ModelConfig &config, const uint8_t *peer_mac);

/** @brief Records an immediate failure returned by esp_now_send for a live-control frame. */
void record_live_control_send_result(esp_err_t result);

/** @brief Emits a rate-limited summary of ESP-NOW MAC send results when diagnostics are enabled. */
void update_espnow_diagnostics(void);
#pragma endregion

#endif // ESPNOW_COMM_H