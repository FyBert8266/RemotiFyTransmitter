/**
 * @file audio_module.cpp
 * @version 1.0.0
 * @brief Implementierung des Audio- und Buzzer-Subsystems mit DAC und Enable-Steuerung (v1.0.0).
 * @Version_info v1.0.0: Implementierung der Pin-Konfiguration (IO26 / IO4 low-active), Event-Filter und VRAM-Diagnose.
 */


#include <Arduino.h>
#include <lvgl.h>
#include "audio_module.h"
#include "SerialInterface.h"

// Interner Zustand
static bool audio_system_enabled = true;

/**
 * @brief Gibt den aktuellen VRAM-Speicherstatus von LVGL im seriellen Monitor aus.
 * @param tag Bezeichner fuer den Aufrufkontext.
 */
static void print_vram_status_audio(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Speicherauslastung: %d%% | Gesamt: %d Bytes | Frei: %d Bytes\n",
                tag, (int)mon.used_pct, (int)mon.total_size, (int)mon.free_size);
  }
}

/**
 * @brief Initialisiert das Audio-Subsystem (Pins IO26 und IO4).
 */
void init_audio_module(void) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Audio)) {
  SerialInterface::printDebug(SerialInterface::Module::Audio, "[AUDIO] Initialisiere Audio-Subsystem...");
  }

  // IO4 als Ausgang fÃ¼r Audio-Enable konfigurieren
  pinMode(AUDIO_EN_PIN, OUTPUT);
  // Low-Level = Enable (wie gewÃ¼nscht: LOW schaltet den VerstÃ¤rker ein)
  digitalWrite(AUDIO_EN_PIN, LOW); 

  // IO26 als DAC-Ausgang vorbereiten
  pinMode(AUDIO_DAC_PIN, ANALOG);

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Audio)) {
  SerialInterface::printDebug(SerialInterface::Module::Audio, "[AUDIO] Audio-Modul betriebsbereit (DAC: IO26, Enable: IO4 [Low-Active]).");
  }

  print_vram_status_audio("INIT_AUDIO");
}

/**
 * @brief Schaltet das Audio-Subsystem via IO4 ein oder aus.
 * @param enable true fÃ¼r Aktivierung (LOW), false fÃ¼r Deaktivierung (HIGH).
 */
void set_audio_enabled(bool enable) {
  audio_system_enabled = enable;
  digitalWrite(AUDIO_EN_PIN, enable ? LOW : HIGH);
  
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Audio)) {
  SerialInterface::printDebug(SerialInterface::Module::Audio, "[AUDIO] Audio-Ausgabe ist nun %s.", enable ? "AKTIVIERT" : "DEAKTIVIERT");
  }
}

/**
 * @brief Spielt einen einfachen Ton Ã¼ber den DAC-Pin aus (oder erzeugt ein Audiosignal).
 * @param frequency Frequenz in Hz (simuliert/erzeugt).
 * @param duration_ms Dauer in Millisekunden.
 */
void play_tone(uint32_t frequency, uint32_t duration_ms) {
  if (!audio_system_enabled) return;

  // Da tone() auf ESP32 standardmÃ¤ÃŸig Ã¼ber LEDC lÃ¤uft, nutzen wir hier einen sauberen Wrapper
  tone(AUDIO_DAC_PIN, frequency, duration_ms);
  
}

/**
 * @brief Triggert akustisches Feedback basierend auf System-Events.
 * @param event_id ID des Audio-Events (z.B. AUDIO_EVENT_MODEL_SWITCH).
 */
void trigger_audio_feedback(uint8_t event_id) {
  if (!audio_system_enabled) return;

  switch (event_id) {
    case AUDIO_EVENT_MODEL_SWITCH:
      play_tone(1200, 80);
      delay(90);
      play_tone(1800, 120);
      break;
    case AUDIO_EVENT_SWITCH:
      play_tone(1000, 50);
      break;
    case AUDIO_EVENT_CLICK:
      play_tone(800, 30);
      break;
    case AUDIO_EVENT_WARNING:
      play_tone(400, 200);
      delay(100);
      play_tone(400, 200);
      break;
    default:
      play_tone(1000, 40);
      break;
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Audio)) {
  SerialInterface::printDebug(SerialInterface::Module::Audio, "[AUDIO] Feedback-Ton gespielt fuer Event: 0x%02X", event_id);
  }
}