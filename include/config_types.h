/**
 * @file config_types.h
 * @version 1.0.4
 * @brief Zentrale Strukturdefinitionen fuer Modell-, Kanal- und Kalibrierungskonfigurationen (v1.0.4).
 * @Version_info v1.0.4: Ergaenzung von rssi und configMismatch in TelemetryMessage zur binaeren Kompatibilitaet mit dem Empfaenger.
 */

#ifndef CONFIG_TYPES_H
#define CONFIG_TYPES_H

#include <Arduino.h>
#include <esp_heap_caps.h>

#define MAX_CHANNELS 24
#define MAX_NAME_LEN 16

#ifndef TRANSMITTER_NAME_LEN
#define TRANSMITTER_NAME_LEN 16
#endif

#define MAX_PAIRED_MODELS 20  // Erweitert auf bis zu 20 speicherbare Empfänger
#define MAX_VISIBLE_TOP_BAR 5 // Maximal 5 gleichzeitig in der Top-Bar sichtbare Slots

// ESP-NOW Nachrichtentypen
#define MSG_TYPE_CONFIG_DATA     0x01
#define MSG_TYPE_CONFIG_REQ      0x02
#define MSG_TYPE_PAIRING         0x03
#define MSG_TYPE_RECEIVER_STATUS 0x04
#define MSG_TYPE_TELEMETRY       0x05
#define MSG_TYPE_LIVE_DATA       0x99

#define PAIRING_ACTION_DISCOVER  0x01
#define PAIRING_ACTION_CONFIRM   0x02
#define PAIRING_ACTION_ACK       0x03
#define PAIRING_PROTOCOL_VERSION 1


// ============================================================
// ENUMS & KONSTANTEN
// ============================================================

enum ChannelType : uint8_t { 
  TYPE_ANALOG = 0,
  TYPE_DIGITAL,
  TYPE_MULTI_STATE 
};

enum ChannelBehaviour : uint8_t { 
  BEHAVIOUR_RESTORE_ZERO = 0,
  BEHAVIOUR_STICK,
  BEHAVIOUR_MOMENTARY,
  BEHAVIOUR_TOGGLE 
};

enum CurveType : uint8_t { 
  CURVE_LINEAR = 0,
  CURVE_PROGRESSIVE,
  CURVE_DEGRESSIVE 
};

enum FailsafeCondition : uint8_t { 
  FAILSAFE_HOLD = 0,
  FAILSAFE_VALUE 
};

// // ESP-NOW Nachrichtentypen
// #define MSG_TYPE_CONFIG_DATA 0x01
// #define MSG_TYPE_CONFIG_REQ  0x02

// ============================================================
// DATENSTRUKTUREN (Packed für exakte Binärübertragung via Funk)
// ============================================================

struct __attribute__((packed)) ChannelConfig {
  char name[MAX_NAME_LEN];
  uint8_t hardwareInputChannel;
  uint8_t type;
  uint8_t behaviour;
  uint8_t curveType;
  uint8_t failsafeType;
  int8_t curveGain;
  int8_t constrainMin;
  int8_t constrainMax;
  int8_t trim;
  uint8_t dampingForward;
  uint8_t dampingReturn;
  int16_t failsafeValue;
  uint8_t deadband;
  uint8_t invert : 1;
  uint8_t behaviourIsChannel : 1;
  uint8_t reserved : 6;
  uint8_t reserved2;
  char states[32];          // Kommagetrennte Liste der Zustaende (z.B. "R2,R1,N,1,2,3,4,5,6" fuer Ganganzeigen)
};

struct __attribute__((packed)) ModelConfig {
  char modelName[MAX_NAME_LEN];
  uint8_t channelCount;
  ChannelConfig channels[MAX_CHANNELS];
};



struct __attribute__((packed)) LiveControlMessage {
  uint8_t packetType = 0x99;
  uint8_t channelCount;
  uint16_t channelValues[MAX_CHANNELS];
};

/**
 * @brief Struktur fuer den Empfaenger-Status (Multi-Sender Lock / IS_BUSY).
 */
struct __attribute__((packed)) ReceiverStatusMessage {
  uint8_t packetType = MSG_TYPE_RECEIVER_STATUS;
  bool isOccupied;
  char activeTransmitterName[16];
};

/**
 * @brief Telemetriedaten-Paket vom Empfaenger (Modell) zurück zum Sender (mit führendem packetType).
 */
struct __attribute__((packed)) TelemetryMessage {
  uint8_t packetType = MSG_TYPE_TELEMETRY; // <--- NEU: Einheitliches Typ-Byte an erster Stelle
  float voltage;          // Aktuelle Akkuspannung in Volt
  float percent;          // Fuellstand in Prozent (0.0 bis 100.0)
  uint32_t remainingMins; // Geschaetzte Restlaufzeit in Minuten (ganzzahlig)
  float pitch;            // Nickwinkel des Fahrzeugs in Grad
  float roll;             // Rollwinkel des Fahrzeugs in Grad
  uint8_t currentGear;    // Eingelegter Gang (z.B. fuer die Dashboard-Gang-Leiste)
  uint8_t lightState;     // Status der Beleuchtung (z.B. Bits fuer Abblendlicht, Fernlicht etc.)
  uint8_t configReceived : 1;    // Flag 1: True für 2s nach erfolgreichem Config-Empfang
  uint8_t liveDataReceived : 1;  // Flag 2: True, wenn seit letzter Telemetrie Live-Daten ankamen
  uint8_t failsafeActive : 1;    // Flag 3: True, wenn Failsafe aktiv war/ist
  uint8_t configMismatch : 1;    // Flag 4: Konfigurations-Inkompatibilität
  uint8_t reserved : 4;          // Rest-Bits auffüllen für Bitfield-Sicherheit
  int8_t rssi;            // Empfangsstaerke (RSSI) des Live-Control-Pakets am Empfaenger
};

/**
 * @brief Struktur fuer ein gepaires Modell / Empfaenger (LittleFS-Persistenz).
 */
struct PairedModelInfo {
  char modelName[MAX_NAME_LEN];
  uint8_t macAddress[6];
  ModelConfig config; // Modellkonfiguration direkt am gepairten Modell speichern
  bool isPaired;            // Gibt an, ob dieser Slot belegt/gepaart ist
  uint8_t sortOrder;        // Aktivierungs- und Anzeigepriorität (0 bis 19)
  bool isOccupiedByOther;
  char occupantTransmitterName[TRANSMITTER_NAME_LEN];

  
  // Laufzeit-Telemetrie & Status (nicht persistent)
  TelemetryMessage telemetry;
  bool hasTelemetry;
  unsigned long lastTelemetryTime;
};

struct __attribute__((packed)) ESPNowTransferMessage {
  uint8_t packetType;
  uint8_t chunkIndex;
  uint8_t totalChunks;
  uint8_t dataLength;
  uint8_t payload[240];
};

// Kalibrierungsstrukturen für analoge Achsen und System-Settings (Gerätespezifisch)
struct __attribute__((packed)) AnalogCalibration {
  int16_t rawMin;
  int16_t rawCenter;
  int16_t rawMax;
};

struct __attribute__((packed)) SystemCalibrationConfig {
  uint32_t magicMarker;       // Zur Validierung der gespeicherten Daten (0xCA11BEEF)
  AnalogCalibration axes[4];  // Index 0 bis 3 für Joystick L X, L Y, R X, R Y
  uint8_t displayBrightness;  // Gespeicherte Displayhelligkeit in Prozent (10-100)
  uint8_t autoDimEnabled;     // NEU: Auto-Dimming Status (0: Aus, 1: Ein)
  char senderName[16];        // NEU: Persistenter Sendername für Kopplung & Status
};

struct __attribute__((packed)) PairingMessage {
  uint8_t packetType;                 // Typ-Byte (MSG_TYPE_PAIRING = 0x03)
  uint8_t action;                     // DISCOVER, CONFIRM oder ACK
  uint8_t protocolVersion;
  uint32_t nonce;                     // Zufallige Pairing-Challenge
  uint8_t receiverMAC[6];             // MAC-Adresse des angefragten Empfaengers
  char modelName[MAX_NAME_LEN];
  char senderName[MAX_NAME_LEN];
};

#endif // CONFIG_TYPES_H