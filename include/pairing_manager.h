#ifndef PAIRING_MANAGER_H
#define PAIRING_MANAGER_H

/**
 * @file pairing_manager.h
 * @version 1.0.0
 * @brief Verwaltung von bis zu 20 gepairten Modellen mit LittleFS-Persistenz, 30s-Pairing-Fenster, Sortierreihenfolge und CYD-Popup-Dialog (v1.0.0).
 * @Version_info v1.0.0: Initiale Erstellung des Pairing- und LittleFS-Persistenz-Managers fuer maximal 20 Empfaenger.
 */

#include <Arduino.h>
#include <lvgl.h>
#include "config_types.h"

#pragma region CONSTANTS_AND_DEFINES
#define PAIRING_STORAGE_FILE "/paired_models.bin"
#define PAIRING_WINDOW_DURATION_MS 30000UL // 30 Sekunden Pairing-Fenster
#pragma endregion

#pragma region DATA_STRUCTURES
/**
 * @brief Persistente Struktur zum Speichern der gepairten Modelle im LittleFS (ohne Laufzeit-Telemetrie).
 */
struct __attribute__((packed)) PersistedModelEntry {
  char modelName[MAX_NAME_LEN];
  uint8_t macAddress[6];
  ModelConfig config; // Modellkonfiguration direkt am gepairten Modell speichern
  bool isPaired;
  uint8_t sortOrder;
};
#pragma endregion

#pragma region FUNCTION_PROTOTYPES
/**
 * @brief Initialisiert den Pairing-Manager und laedt gespeicherte Modelle aus LittleFS.
 */
void init_pairing_manager(void);

/**
 * @brief Laedt alle gepairten Modelle (bis zu 20) aus LittleFS in das globale pairedModels-Array.
 */
void load_paired_models_from_fs(void);

/**
 * @brief Laedt alle gepairten Modelle (bis zu 20) in das ESPNOW-Register.
 */
void register_paired_macs(void);

/**
 * @brief Speichert den aktuellen Stand von pairedModels persistent in LittleFS.
 */
void save_paired_models_to_fs(void);

/**
 * @brief Startet das 30-sekundige Pairing-Fenster fuer neue Empfaenger.
 */
void start_pairing_window(void);

/**
 * @brief Beendet das Pairing-Fenster vorzeitig.
 */
void stop_pairing_window(void);

/**
 * @brief Gibt zurueck, ob das 30s-Pairing-Fenster aktuell aktiv ist.
 */
bool is_pairing_window_active(void);

/**
 * @brief Zyklische Laufzeit-Ueberwachung (Timeout des 30s-Fensters pruefen).
 */
void update_pairing_manager(void);

/**
 * @brief Wird aus espnow_comm aufgerufen, wenn ein Paket von einer unbekannten MAC empfangen wird.
 * @param mac Zeiger auf die 6-Byte MAC-Adresse des unbekannten Empfaengers.
 * @param detectedModelName Vom Empfaenger gemeldeter Modellname (falls verfuegbar, sonst Fallback).
 */
void handle_discovered_peer_during_pairing(const uint8_t *mac, const char *detectedModelName);

/**
 * @brief Aktualisiert den Modellnamen eines gepairten Empfaengers und speichert bei Aenderung in LittleFS.
 * @param modelIndex Index im globalen pairedModels-Array.
 * @param newName Neuer Modellname vom Empfaenger.
 */
void update_paired_model_name_if_changed(int modelIndex, const char *newName);

/**
 * @brief Entfernt ein gepairtes Modell dauerhaft und komprimiert die Sortierreihenfolge.
 * @param modelIndex Index des zu loeschenden Modells.
 */
void delete_paired_model(int modelIndex);

/**
 * @brief Verschiebt ein gepairtes Modell in der Aktivierungsreihenfolge nach oben (-1) oder unten (+1).
 * @param modelIndex Index des zu verschiebenden Modells.
 * @param direction -1 fuer hoehere Prioritaet (nach oben), +1 fuer niedrigere Prioritaet (nach unten).
 */
void move_paired_model_order(int modelIndex, int direction);
bool ensure_paired_models_allocated(void);
#pragma endregion

#endif // PAIRING_MANAGER_H