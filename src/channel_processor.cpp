/**
 * @file channel_processor.cpp
 * @version 1.0.4
 * @brief Implementierung der Signalverarbeitung im PWM-Standard (1000 bis 2000 us) (v1.0.4).
 * @Version_info v1.0.4: Sicherheits-Fixierung des WINCH-Kanals auf 1500 us bei aktivem Winden-Sperrschalter sowie einheitliche Debug-/VRAM-Struktur.
 */


#include <Arduino.h>
#include <string.h>
#include <lvgl.h>
#include "SerialInterface.h"
#include "config_types.h"
#include "channel_processor.h"
#include "hardware_io.h"
#include "dashboard_page.h"

// Interner Puffer fuer die Daempfung (gleitender Mittelwert pro Kanal in PWM-Einheiten)
static float lastProcessedValues[MAX_CHANNELS] = {1500.0f};

/**
 * @brief Gibt den aktuellen LVGL-Speicherstatus aus (einheitliche VRAM-Diagnose).
 * @param tag Bezeichner fuer die Ausgabe.
 */
static void print_vram_status(const char *tag) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Memory)) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  SerialInterface::printDebug(SerialInterface::Module::Memory, "[VRAM DIAGNOSE] [%s] Auslastung: %d%% | Groesster freier Block: %u Bytes\n",
                tag,
                (int)mon.used_pct,
                (unsigned int)mon.free_biggest_size);
  }
}

#pragma region CHANNEL_PIPELINE
/**
 * @brief Berechnet den verarbeiteten PWM-Kanalwert (1000 bis 2000 us) gemaess ChannelConfig.
 */
uint16_t calculateProcessedChannelValue(const ModelConfig &config, int channel_idx, uint16_t raw_pwm) {
  if (channel_idx < 0 || channel_idx >= config.channelCount || channel_idx >= MAX_CHANNELS) {
    return 1500; // Neutralwert bei ungueltigem Index
  }

  const ChannelConfig &chan = config.channels[channel_idx];

  // Bei aktivem Winden-Sperrschalter wird der WINCH-Kanal strikt auf 1500 us gehalten
  if (strcmp(chan.name, "WINCH") == 0 && is_winch_locked()) {
    lastProcessedValues[channel_idx] = 1500.0f;
    return 1500;
  }

  // Sonderbehandlung fuer Digitalkanaele
  if (chan.type == TYPE_DIGITAL) {
    bool state = (raw_pwm > 1500);

    // Konvertierung von constrainMin / constrainMax (-100 bis +100%) in PWM (1000 bis 2000 us)
    // constrainMin/Max liegen als Prozent vor (z.B. -100 bis +100 oder 0 bis 100)
    int min_pct = (chan.constrainMin != 0 || chan.constrainMax != 0) ? chan.constrainMin : -100;
    int max_pct = (chan.constrainMin != 0 || chan.constrainMax != 0) ? chan.constrainMax : 100;

    uint16_t pwm_min = 1500 + (min_pct * 5); // z.B. -100% -> 1000 us
    uint16_t pwm_max = 1500 + (max_pct * 5); // z.B. +100% -> 2000 us

    uint16_t digital_pwm = state ? pwm_max : pwm_min;
    if (chan.invert) {
      digital_pwm = state ? pwm_min : pwm_max;
    }
    return digital_pwm;
  }

  // Schritt 1: Eingangswert auf Abweichung von der Mitte (1500 us) abbilden (-500 bis +500)
  float val = (float)raw_pwm - 1500.0f;

  // Schritt 2: Deadband (Totzone) anwenden (chan.deadband in Prozent -> 1% = 5 us)
  if (chan.deadband > 0) {
    float db = (float)chan.deadband * 5.0f;
    if (abs(val) <= db) {
      val = 0.0f;
    } else {
      if (val > 0) {
        val = (val - db) * (500.0f / (500.0f - db));
      } else {
        val = (val + db) * (500.0f / (500.0f - db));
      }
    }
  }

  // Schritt 3: Invertierung
  if (chan.invert) {
    val = -val;
  }

  // Schritt 4: Trimmung addieren (chan.trim in Prozent -> 1% = 5 us Offset)
  val += (float)chan.trim * 5.0f;

  // Schritt 5: Kurvenform (Expo / Dual Rate) anwenden
  if (chan.curveType == 1) { // Progressiv
    float normalized = val / 500.0f; // -1.0 bis 1.0
    float gain = (float)chan.curveGain / 100.0f;
    val = 500.0f * (normalized * normalized * normalized * gain + normalized * (1.0f - gain));
  } else if (chan.curveType == 2) { // Degressiv
    float normalized = val / 500.0f;
    float gain = (float)chan.curveGain / 100.0f;
    val = 500.0f * (copysign(sqrt(abs(normalized)), normalized) * gain + normalized * (1.0f - gain));
  }

  // Schritt 6: Constraints (Begrenzung auf constrainMin / constrainMax in Prozent)
  float minLimit = -500.0f * ((float)abs(chan.constrainMin) / 100.0f);
  float maxLimit = 500.0f * ((float)abs(chan.constrainMax) / 100.0f);
  if (chan.constrainMin == 0 && chan.constrainMax == 0) {
    minLimit = -500.0f;
    maxLimit = 500.0f;
  }
  if (val < minLimit) val = minLimit;
  if (val > maxLimit) val = maxLimit;

  // Schritt 7: Daempfung (Damping Forward / Return)
  float current_abs_dev = abs(val);
  float last_abs_dev = abs(lastProcessedValues[channel_idx] - 1500.0f);
  float damping = (current_abs_dev > last_abs_dev) ? (float)chan.dampingForward : (float)chan.dampingReturn;

  float target_pwm = 1500.0f + val;
  if (damping > 0) {
    float alpha = 1.0f / (1.0f + (damping * 0.1f));
    target_pwm = lastProcessedValues[channel_idx] + alpha * (target_pwm - lastProcessedValues[channel_idx]);
  }
  lastProcessedValues[channel_idx] = target_pwm;

  // Schritt 8: Finaler PWM-Ausgangswert (1000 bis 2000 us)
  uint16_t final_pwm = (uint16_t)round(target_pwm);

  if (final_pwm < 1000) final_pwm = 1000;
  if (final_pwm > 2000) final_pwm = 2000;

  // Letzte Sperrpruefung fuer WINCH-Kanal
  if (strcmp(chan.name, "WINCH") == 0 && is_winch_locked()) {
    lastProcessedValues[channel_idx] = 1500.0f;
    final_pwm = 1500;
  }

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Channels)) {
  static unsigned long lastDebugLog = 0;
  if (millis() - lastDebugLog > 500 && SerialInterface::isChannelEnabled(channel_idx)) {
    lastDebugLog = millis();
    SerialInterface::printDebug(SerialInterface::Module::Channels, "[DEBUG] Kanal %d [%s] -> Roh-PWM: %u us | Verarbeitet-PWM: %u us\n",
                  channel_idx + 1, chan.name, raw_pwm, final_pwm);
  }
  }

  return final_pwm;
}
#pragma endregion