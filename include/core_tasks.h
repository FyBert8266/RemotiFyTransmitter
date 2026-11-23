/**
 * @file core_tasks.h
 * @version 1.0.0
 * @brief Header-Datei fuer die FreeRTOS Multi-Core-Task-Verwaltung (Core 0 Echtzeit / Core 1 UI) (v1.0.0).
 * @Version_info v1.0.0: Ersterstellung des Multi-Core-Moduls zur Trennung von Hardware-IO/ESP-NOW (Core 0) und LVGL/LittleFS (Core 1).
 */

#ifndef CORE_TASKS_H
#define CORE_TASKS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config_types.h"

#pragma region EXTERNS_AND_LOCKS
// Mutex zur thread-sicheren Synchronisation gemeinsamer Datenstrukturen zwischen Core 0 und Core 1
extern portMUX_TYPE configMux;
#pragma endregion

#pragma region FUNCTION_PROTOTYPES
/**
 * @brief Initialisiert die Synchronisations-Primitive und startet den Echtzeit-Task auf Core 0.
 */
void init_core_tasks(void);

/**
 * @brief Gibt die aktuelle Ausfuehrungsfrequenz (Hz) des Core-0 Echtzeit-Tasks zurueck.
 * @return Frequenz in Hz.
 */
float get_core0_task_hz(void);
#pragma endregion

#endif // CORE_TASKS_H