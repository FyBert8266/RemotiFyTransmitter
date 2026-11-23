#ifndef AUDIO_MODULE_H
#define AUDIO_MODULE_H

/**
 * @file audio_module.h
 * @version 1.0.0
 * @brief Header-Datei fuer das Audio- und Buzzer-Subsystem (DAC auf IO26, Enable auf IO4, low-active) (v1.0.0).
 * @Version_info v1.0.0: Initiale Erstellung des Audio-Moduls mit Hardware-Pins und Event-Tönen.
 */

#include <Arduino.h>

// Pin-Definitionen laut Hardware-Vorgabe
#define AUDIO_DAC_PIN    26  // DAC Output Signal
#define AUDIO_EN_PIN     4   // Audio Enable (Low-Level aktiv)

// Audio Event IDs für akustisches Feedback
#define AUDIO_EVENT_MODEL_SWITCH 0x01
#define AUDIO_EVENT_SWITCH       0x02
#define AUDIO_EVENT_CLICK        0x03
#define AUDIO_EVENT_WARNING      0x04

// Funktionsprototypen
void init_audio_module(void);
void set_audio_enabled(bool enable);
void trigger_audio_feedback(uint8_t event_id);
void play_tone(uint32_t frequency, uint32_t duration_ms);

#endif // AUDIO_MODULE_H