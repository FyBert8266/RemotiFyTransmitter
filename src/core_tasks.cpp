/**
 * @file core_tasks.cpp
 * @version 1.0.1
 * @brief Implementierung des Echtzeit-Tasks auf Core 0 (I2C-Polling, Channel-Processor, ESP-NOW) (v1.0.1).
 * @Version_info v1.0.1: Kopplung des WINCH-Kanals an Sperrzustand (1500 us Fixierung), 50%-Button-Auslenkung und groessere Auslenkung zwischen Button und Hardware-IO.
 */


#include "core_tasks.h"
#include "hardware_io.h"
#include "channel_processor.h"
#include "espnow_comm.h"
#include "dashboard_page.h"
#include "SerialInterface.h"
#include <esp_now.h>
#include <stdlib.h>
#include <string.h>

#pragma region GLOBALS_AND_STATICS
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t core0TaskHandle = NULL;
static volatile float core0_hz = 0.0f;
static ModelConfig localConfigCopy;
static ModelConfig fingerprintConfig;

// Externer Bezug auf den virtuellen Rohwert-Puffer aus dem Dashboard
extern uint16_t virtual_channel_raw[MAX_CHANNELS];

// Externe Referenzen aus dem Haupt-Sketch und espnow_comm
extern ModelConfig currentConfig;
extern uint8_t receiverMAC[];

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
#pragma endregion

#pragma region CORE0_REALTIME_TASK
/**
 * @brief FreeRTOS Task auf Core 0 fuer deterministische Hardware-Abfrage und Funkuebertragung.
 * @param pvParameters Optionaler Task-Parameter (ungenutzt).
 */
static void core0_realtime_task(void *pvParameters) {
  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Core)) {
  SerialInterface::printDebug(SerialInterface::Module::Core, "[CORE 0] Echtzeit-Task gestartet auf Core %d.", xPortGetCoreID());
  }
  print_vram_status("Core0 RT Task Start");

  unsigned long lastSend = 0;
  uint32_t cycle_counter = 0;
  unsigned long last_hz_check = millis();
  uint32_t liveSequence = 0;
  uint32_t cachedConfigFingerprint = 0;
  bool configFingerprintInitialized = false;

  for (;;) {
    // 1. Nicht-blockierendes I2C-Caching der analogen Achsen (Round-Robin)
    update_hardware_cache();

    // 2. Zyklisches Berechnen und Senden der Live-Steuerdaten alle 15ms (~66 Hz)
    unsigned long now = millis();
    if (now - lastSend >= 15) {
      lastSend = now;

      // Thread-sicheres Kopieren der aktuellen Konfiguration von Core 1
      portENTER_CRITICAL(&configMux);
      memcpy(&localConfigCopy, &currentConfig, sizeof(ModelConfig));
      portEXIT_CRITICAL(&configMux);

      LiveControlMessage liveData;
      liveData.packetType = MSG_TYPE_LIVE_DATA;
      liveData.channelCount = localConfigCopy.channelCount;
      liveData.protocolVersion = LIVE_CONTROL_PROTOCOL_VERSION;
      liveData.sequence = ++liveSequence;
      if (!configFingerprintInitialized ||
          memcmp(&fingerprintConfig, &localConfigCopy, sizeof(ModelConfig)) != 0) {
        fingerprintConfig = localConfigCopy;
        cachedConfigFingerprint = model_config_fingerprint(localConfigCopy);
        configFingerprintInitialized = true;
      }
      liveData.configFingerprint = cachedConfigFingerprint;

      for (int i = 0; i < localConfigCopy.channelCount && i < MAX_CHANNELS; i++) {
        uint8_t hw_id = localConfigCopy.channels[i].hardwareInputChannel;
        uint16_t raw_pwm = 1500;

        if (hw_id >= 1 && hw_id <= 4) {
          int16_t raw_val = read_hardware_raw(hw_id);
          uint8_t db = localConfigCopy.channels[i].deadband;
          raw_pwm = read_hardware_calibrated_pwm(hw_id, raw_val, db);
        } else if (hw_id >= 5 && hw_id <= 7) {
          bool digital_state = (now % 2000 > 1000);
          raw_pwm = digital_state ? 2000 : 1000;
        } else {
          // hw_id == 0: Virtueller Geber (UI-Schalter / Dashboard Lichter)
          raw_pwm = virtual_channel_raw[i];
        }

        // Sonderlogik fuer den WINCH-Kanal: Sperre (1500 us) oder groessere Auslenkung (50%-Buttons vs. HW-IO)
        if (strcmp(localConfigCopy.channels[i].name, "WINCH") == 0) {
          if (is_winch_locked()) {
            raw_pwm = 1500;
          } else {
            uint16_t btn_pwm = get_winch_button_target_pwm(i);
            uint16_t hw_winch_pwm = get_winch_hardware_pwm(i);
            int btn_dev = abs((int)btn_pwm - 1500);
            int hw_dev = abs((int)hw_winch_pwm - 1500);
            raw_pwm = (btn_dev >= hw_dev) ? btn_pwm : hw_winch_pwm;
          }
        }

        liveData.channelValues[i] = calculateProcessedChannelValue(localConfigCopy, i, raw_pwm);

        if (strcmp(localConfigCopy.channels[i].name, "WINCH") == 0 && is_winch_locked()) {
          liveData.channelValues[i] = 1500;
        }
      }

      // Ziel-MAC aus aktiver Modellauswahl oder Fallback receiverMAC verwenden
      const uint8_t *targetMac = (pairedModels != NULL &&
                                  activeModelIndex >= 0 &&
                                  activeModelIndex < pairedModelCount &&
                                  activeModelIndex < MAX_PAIRED_MODELS)
                                   ? pairedModels[activeModelIndex].macAddress
                                   : receiverMAC;

      bool validTarget = false;
      for (uint8_t i = 0; i < 6; ++i) {
        if (targetMac[i] != 0) {
          validTarget = true;
          break;
        }
      }
      if (validTarget && liveData.channelCount > 0 && liveData.channelCount <= MAX_CHANNELS) {
        esp_err_t sendResult = esp_now_send(targetMac, (uint8_t *)&liveData, sizeof(LiveControlMessage));
        record_live_control_send_result(sendResult);
      }
    }

    // 3. Frequenz-Monitoring fuer Core 0
    cycle_counter++;
    if (now - last_hz_check >= 1000) {
      core0_hz = (float)cycle_counter * 1000.0f / (now - last_hz_check);
      cycle_counter = 0;
      last_hz_check = now;
    }

    // 4. Wichtig: Rechenzeit fuer WiFi/ESP-NOW-Stack und Idle-Watchdog auf Core 0 freigeben
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
#pragma endregion

#pragma region INITIALIZATION_AND_GETTERS
void init_core_tasks(void) {
  xTaskCreatePinnedToCore(
    core0_realtime_task,   // Task-Funktion
    "Core0_RT_Task",       // Task-Name
    4096,                  // Stack-Groesse in Bytes
    NULL,                  // Parameter
    2,                     // Prioritaet (ueber Idle, unter kritischen WiFi-Systemtasks)
    &core0TaskHandle,      // Task-Handle
    0                      // Exklusiv an Core 0 pinnen
  );

  if (SerialInterface::isModuleEnabled(SerialInterface::Module::Core)) {
  SerialInterface::printDebug(SerialInterface::Module::Core, "[SYSTEM] FreeRTOS Multi-Core-Trennung aktiv (Core 0: RT/RF | Core 1: LVGL/FS).");
  }
}

float get_core0_task_hz(void) {
  return core0_hz;
}
#pragma endregion