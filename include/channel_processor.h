/**
 * @file channel_processor.h
 * @version 1.0.3
 * @brief Header-Datei fuer die Kanalwert-Berechnung und Signalverarbeitung (v1.0.3).
 * @Version_info v1.0.3: Umstellung der Pipeline auf den PWM-Standard (1000 bis 2000 us) zur Erhaltung der vollen Signalaufloesung.
 */

#ifndef CHANNEL_PROCESSOR_H
#define CHANNEL_PROCESSOR_H

#include <Arduino.h>
#include "config_types.h"

/**
 * @brief Berechnet den finalen PWM-Kanalwert (1000 bis 2000 us) unter Beruecksichtigung aller Kanal-Konfigurationen.
 * @param config Modellkonfiguration, zu der der Kanal gehoert.
 * @param channel_idx Index des Kanals in der Modellkonfiguration.
 * @param raw_pwm Roher PWM-Eingangswert (1000 bis 2000 us) von read_hardware_calibrated_pwm.
 * @return uint16_t Finaler PWM-Kanalwert (1000 bis 2000 us).
 */
uint16_t calculateProcessedChannelValue(const ModelConfig &config, int channel_idx, uint16_t raw_pwm);

#endif // CHANNEL_PROCESSOR_H