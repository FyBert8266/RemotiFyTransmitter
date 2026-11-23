/**
 * @file SerialInterface.cpp
 * @brief Runtime-configurable serial diagnostics, profiles, and system information.
 */

#include "SerialInterface.h"
#include "espnow_comm.h"

#include <LittleFS.h>
#include <atomic>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr char kActiveConfigPath[] = "/serial_config.bin";
constexpr char kConfigDirectory[] = "/serial_cfg";
constexpr char kConfigMagic[] = "SDBG";
constexpr uint16_t kConfigVersion = 1;
constexpr size_t kCommandBufferSize = 96;
constexpr uint32_t kValidModuleMask = static_cast<uint32_t>(SerialInterface::Module::All);

struct PersistedConfig {
  char magic[4];
  uint16_t version;
  uint16_t size;
  uint32_t moduleMask;
  int8_t targetChannel;
  uint8_t reserved[3];
  uint32_t checksum;
};

std::atomic<uint32_t> activeModuleMask(kValidModuleMask);
std::atomic<int8_t> targetChannel(-1);
char commandBuffer[kCommandBufferSize];
size_t commandLength = 0;
bool commandOverflow = false;

uint32_t calculateChecksum(const PersistedConfig& config) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&config);
  uint32_t checksum = 2166136261u;
  for (size_t i = 0; i < offsetof(PersistedConfig, checksum); ++i) {
    checksum = (checksum ^ bytes[i]) * 16777619u;
  }
  return checksum;
}

bool readConfigFile(const char* path, PersistedConfig& config) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;

  const bool validSize = file.size() == sizeof(config);
  const size_t bytesRead = validSize ? file.read(reinterpret_cast<uint8_t*>(&config), sizeof(config)) : 0;
  file.close();

  return bytesRead == sizeof(config) &&
         memcmp(config.magic, kConfigMagic, sizeof(config.magic)) == 0 &&
         config.version == kConfigVersion &&
         config.size == sizeof(config) &&
         (config.moduleMask & ~kValidModuleMask) == 0 &&
         config.targetChannel >= -1 &&
         config.targetChannel < MAX_CHANNELS &&
         config.checksum == calculateChecksum(config);
}

void makeConfig(PersistedConfig& config) {
  memset(&config, 0, sizeof(config));
  memcpy(config.magic, kConfigMagic, sizeof(config.magic));
  config.version = kConfigVersion;
  config.size = sizeof(config);
  config.moduleMask = activeModuleMask.load(std::memory_order_relaxed);
  config.targetChannel = targetChannel.load(std::memory_order_relaxed);
  config.checksum = calculateChecksum(config);
}

bool writeConfigFile(const char* path) {
  PersistedConfig config;
  makeConfig(config);

  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) return false;
  const size_t bytesWritten = file.write(reinterpret_cast<const uint8_t*>(&config), sizeof(config));
  file.flush();
  file.close();
  if (bytesWritten != sizeof(config)) return false;

  PersistedConfig verification;
  return readConfigFile(path, verification) &&
         verification.checksum == config.checksum;
}

void printFormatted(const char* format, va_list args) {
  char buffer[192];
  vsnprintf(buffer, sizeof(buffer), format, args);
  Serial.println(buffer);
}

bool parseOnOff(const char* value, bool& enabled) {
  if (strcmp(value, "ON") == 0 || strcmp(value, "1") == 0) {
    enabled = true;
    return true;
  }
  if (strcmp(value, "OFF") == 0 || strcmp(value, "0") == 0) {
    enabled = false;
    return true;
  }
  return false;
}

const char* moduleName(SerialInterface::Module module) {
  switch (module) {
    case SerialInterface::Module::System: return "SYSTEM";
    case SerialInterface::Module::Memory: return "MEMORY";
    case SerialInterface::Module::Hardware: return "HARDWARE";
    case SerialInterface::Module::Channels: return "CHANNELS";
    case SerialInterface::Module::Telemetry: return "TELEMETRY";
    case SerialInterface::Module::EspNow: return "ESPNOW";
    case SerialInterface::Module::Pairing: return "PAIRING";
    case SerialInterface::Module::UI: return "UI";
    case SerialInterface::Module::Battery: return "BATTERY";
    case SerialInterface::Module::Audio: return "AUDIO";
    case SerialInterface::Module::Models: return "MODELS";
    case SerialInterface::Module::Core: return "CORE";
    case SerialInterface::Module::All: return "ALL";
  }
  return "UNKNOWN";
}

const SerialInterface::Module modules[] = {
  SerialInterface::Module::System,
  SerialInterface::Module::Memory,
  SerialInterface::Module::Hardware,
  SerialInterface::Module::Channels,
  SerialInterface::Module::Telemetry,
  SerialInterface::Module::EspNow,
  SerialInterface::Module::Pairing,
  SerialInterface::Module::UI,
  SerialInterface::Module::Battery,
  SerialInterface::Module::Audio,
  SerialInterface::Module::Models,
  SerialInterface::Module::Core
};
} // namespace

void SerialInterface::begin() {
  commandLength = 0;
  commandOverflow = false;
  if (LittleFS.exists(kActiveConfigPath)) {
    PersistedConfig config;
    if (readConfigFile(kActiveConfigPath, config)) {
      activeModuleMask.store(config.moduleMask, std::memory_order_relaxed);
      targetChannel.store(config.targetChannel, std::memory_order_relaxed);
    } else {
      Serial.println("[SERIAL][ERROR] Gespeicherte Diagnosekonfiguration ist ungueltig; Standardwerte werden verwendet.");
      activeModuleMask.store(kValidModuleMask, std::memory_order_relaxed);
      targetChannel.store(-1, std::memory_order_relaxed);
      if (!persistCurrentConfig()) {
        Serial.println("[SERIAL][ERROR] Standardkonfiguration konnte nicht gespeichert werden.");
      }
    }
  } else if (!persistCurrentConfig()) {
    Serial.println("[SERIAL][ERROR] Diagnosekonfiguration konnte nicht initialisiert werden.");
  }

  printHelp();
}

void SerialInterface::update() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\r') continue;
    if (incoming == '\n') {
      if (commandOverflow) {
        Serial.println("[SERIAL][ERROR] Befehl zu lang; maximal 95 Zeichen.");
      } else if (commandLength > 0) {
        commandBuffer[commandLength] = '\0';
        for (size_t i = 0; i < commandLength; ++i) {
          if (commandBuffer[i] >= 'a' && commandBuffer[i] <= 'z') {
            commandBuffer[i] = static_cast<char>(commandBuffer[i] - 'a' + 'A');
          }
        }
        processCommand(commandBuffer);
      }
      commandLength = 0;
      commandOverflow = false;
      continue;
    }

    if (commandLength + 1 < sizeof(commandBuffer)) {
      commandBuffer[commandLength++] = incoming;
    } else {
      commandOverflow = true;
    }
  }
}

void SerialInterface::processCommand(char* command) {
  char* context = NULL;
  char* first = strtok_r(command, " \t", &context);
  if (first == NULL) return;

  if (strcmp(first, "HELP") == 0 || strcmp(first, "?") == 0) {
    printHelp();
    return;
  }
  if (strcmp(first, "SYSINFO") == 0) {
    printSystemInfo();
    return;
  }
  if (strcmp(first, "MODELINFO") == 0) {
    printModelInfo();
    return;
  }
  if (strcmp(first, "CONFIGS") == 0 || strcmp(first, "LIST") == 0) {
    listConfigs();
    return;
  }
  if (strcmp(first, "SAVE") == 0 || strcmp(first, "LOAD") == 0) {
    char* name = strtok_r(NULL, " \t", &context);
    char* extra = strtok_r(NULL, " \t", &context);
    if (name == NULL || extra != NULL) {
      Serial.printf("[SERIAL][ERROR] Syntax: %s <name>\n", first);
      return;
    }
    const bool success = strcmp(first, "SAVE") == 0 ? saveConfig(name) : loadConfig(name);
    if (!success) {
      Serial.printf("[SERIAL][ERROR] %s '%s' fehlgeschlagen.\n", first, name);
    }
    return;
  }
  if (strcmp(first, "DEBUG") == 0) {
    char* argument = strtok_r(NULL, " \t", &context);
    char* value = strtok_r(NULL, " \t", &context);
    char* extra = strtok_r(NULL, " \t", &context);
    if (argument == NULL) {
      printDebugStatus();
      return;
    }
    if ((strcmp(argument, "ON") == 0 || strcmp(argument, "OFF") == 0) &&
        value == NULL && extra == NULL) {
      activeModuleMask.store(strcmp(argument, "ON") == 0 ? kValidModuleMask : 0,
                             std::memory_order_relaxed);
      if (!persistCurrentConfig()) {
        Serial.println("[SERIAL][ERROR] Diagnosekonfiguration konnte nicht gespeichert werden.");
      } else {
        Serial.printf("[SERIAL] Debug-Ausgabe global %s.\n",
                      activeModuleMask.load(std::memory_order_relaxed) == 0
                        ? "deaktiviert" : "aktiviert");
      }
      return;
    }
    if (strcmp(argument, "STATUS") == 0 && value == NULL && extra == NULL) {
      printDebugStatus();
      return;
    }
    if (strcmp(argument, "SAVE") == 0 || strcmp(argument, "LOAD") == 0) {
      if (value == NULL || extra != NULL) {
        Serial.printf("[SERIAL][ERROR] Syntax: DEBUG %s <name>\n", argument);
        return;
      }
      const bool success = strcmp(argument, "SAVE") == 0 ? saveConfig(value) : loadConfig(value);
      if (!success) {
        Serial.printf("[SERIAL][ERROR] DEBUG %s '%s' fehlgeschlagen.\n", argument, value);
      }
      return;
    }
    if (strcmp(argument, "CONFIGS") == 0 && value == NULL && extra == NULL) {
      listConfigs();
      return;
    }

    bool enabled = false;
    if (value == NULL || extra != NULL || !parseOnOff(value, enabled)) {
      Serial.println("[SERIAL][ERROR] Syntax: DEBUG ON|OFF|STATUS|<module> ON|OFF");
      return;
    }
    if (strcmp(argument, "ALL") == 0) {
      activeModuleMask.store(enabled ? kValidModuleMask : 0, std::memory_order_relaxed);
      if (!persistCurrentConfig()) {
        Serial.println("[SERIAL][ERROR] Konfiguration konnte nicht gespeichert werden.");
      } else {
        Serial.printf("[SERIAL] Debug-Ausgabe global %s.\n", enabled ? "aktiviert" : "deaktiviert");
      }
      return;
    }

    Module module;
    if (!parseModule(argument, module)) {
      Serial.printf("[SERIAL][ERROR] Unbekanntes Modul '%s'. Nutze DEBUG STATUS.\n", argument);
      return;
    }
    setModuleEnabled(module, enabled);
    return;
  }
  if (strcmp(first, "TELEMETRY") == 0 || strcmp(first, "TEL") == 0) {
    char* value = strtok_r(NULL, " \t", &context);
    bool enabled = false;
    if (value == NULL || strtok_r(NULL, " \t", &context) != NULL || !parseOnOff(value, enabled)) {
      Serial.println("[SERIAL][ERROR] Syntax: TELEMETRY ON|OFF");
      return;
    }
    setModuleEnabled(Module::Telemetry, enabled);
    return;
  }
  if (strcmp(first, "CHAN") == 0) {
    char* value = strtok_r(NULL, " \t", &context);
    char* state = strtok_r(NULL, " \t", &context);
    if (value == NULL) {
      Serial.println("[SERIAL][ERROR] Syntax: CHAN <1-24>|OFF [ON|OFF]");
      return;
    }
    if (strcmp(value, "OFF") == 0 && state == NULL) {
      targetChannel = -1;
    } else {
      char* end = NULL;
      const long channel = strtol(value, &end, 10);
      if (end == value || *end != '\0' || channel < 1 || channel > MAX_CHANNELS) {
        Serial.printf("[SERIAL][ERROR] Ungueltiger Kanal. Erlaubt: 1 bis %d.\n", MAX_CHANNELS);
        return;
      }
      if (state != NULL) {
        bool enabled = false;
        if (strtok_r(NULL, " \t", &context) != NULL || !parseOnOff(state, enabled)) {
          Serial.println("[SERIAL][ERROR] Syntax: CHAN <1-24> ON|OFF");
          return;
        }
        if (enabled) {
          targetChannel = static_cast<int8_t>(channel - 1);
        } else if (targetChannel == channel - 1) {
          targetChannel = -1;
        }
      } else {
        targetChannel = static_cast<int8_t>(channel - 1);
      }
    }
    if (!persistCurrentConfig()) {
      Serial.println("[SERIAL][ERROR] Kanal-Debugkonfiguration konnte nicht gespeichert werden.");
    } else {
      Serial.println(targetChannel < 0
        ? "[SERIAL] Kanalfilter deaktiviert."
        : "[SERIAL] Kanalfilter aktualisiert.");
    }
    return;
  }

  Serial.println("[SERIAL][ERROR] Unbekannter Befehl. Nutze HELP.");
}

bool SerialInterface::isGlobalDebugEnabled() {
  return activeModuleMask.load(std::memory_order_relaxed) != 0;
}

bool SerialInterface::isTelemetryDebugEnabled() {
  return isModuleEnabled(Module::Telemetry);
}

bool SerialInterface::isModuleEnabled(Module module) {
  return (activeModuleMask.load(std::memory_order_relaxed) &
          static_cast<uint32_t>(module)) != 0;
}

bool SerialInterface::isChannelEnabled(int channelIndex) {
  const int8_t selectedChannel = targetChannel.load(std::memory_order_relaxed);
  return selectedChannel >= 0 ? selectedChannel == channelIndex : channelIndex == 0;
}

int SerialInterface::getTargetChannel() {
  return targetChannel.load(std::memory_order_relaxed);
}

void SerialInterface::printSystem(const char* format, ...) {
  if (!isModuleEnabled(Module::System)) return;
  va_list args;
  va_start(args, format);
  printFormatted(format, args);
  va_end(args);
}

void SerialInterface::printError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  printFormatted(format, args);
  va_end(args);
}

void SerialInterface::printDebug(const char* format, ...) {
  if (!isModuleEnabled(Module::System)) return;
  va_list args;
  va_start(args, format);
  printFormatted(format, args);
  va_end(args);
}

void SerialInterface::printDebug(Module module, const char* format, ...) {
  if (!isModuleEnabled(module)) return;
  va_list args;
  va_start(args, format);
  printFormatted(format, args);
  va_end(args);
}

void SerialInterface::printTelemetryLog(const char* format, ...) {
  if (!isModuleEnabled(Module::Telemetry)) return;
  va_list args;
  va_start(args, format);
  printFormatted(format, args);
  va_end(args);
}

void SerialInterface::printHelp() {
  Serial.println("\n=== REMOTIFY SERIAL INTERFACE ===");
  Serial.println("HELP / ?                - Befehlsuebersicht");
  Serial.println("DEBUG STATUS            - Status aller Diagnosemodule");
  Serial.println("DEBUG ON|OFF            - Alle Debugmodule ein-/ausschalten");
  Serial.println("DEBUG <MODUL> ON|OFF    - Einzelnes Modul schalten");
  Serial.println("TELEMETRY ON|OFF        - Telemetrie-Diagnose schalten");
  Serial.println("CHAN <1-24> [ON|OFF]    - Kanalfilter setzen; CHAN OFF loescht ihn");
  Serial.println("SAVE <name> / LOAD <name> - Profil speichern/laden");
  Serial.println("CONFIGS                 - Gespeicherte Profile anzeigen");
  Serial.println("SYSINFO                 - Systeminformationen anzeigen");
  Serial.println("MODELINFO               - Aktive Modellkonfiguration");
  Serial.println("Module: SYSTEM MEMORY HARDWARE CHANNELS TELEMETRY ESPNOW PAIRING UI BATTERY AUDIO MODELS CORE");
}

void SerialInterface::printModelInfo() {
  extern ModelConfig currentConfig;
  Serial.println("\n=== AKTIVE MODELL-INFORMATIONEN ===");
  Serial.printf("Modellname:  %s\n", currentConfig.modelName);
  Serial.printf("Kanalanzahl: %u\n", currentConfig.channelCount);
  for (uint8_t i = 0; i < currentConfig.channelCount && i < MAX_CHANNELS; ++i) {
    const ChannelConfig& channel = currentConfig.channels[i];
    Serial.printf("Ch %u: %-16s | HW: %u | Type: %u\n",
                  static_cast<unsigned int>(i + 1), channel.name,
                  channel.hardwareInputChannel, channel.type);
  }
}

void SerialInterface::printSystemInfo() {
  extern ModelConfig currentConfig;
  const uint32_t totalFlash = LittleFS.totalBytes();
  const uint32_t usedFlash = totalFlash > 0 ? LittleFS.usedBytes() : 0;
  const bool linkConnected = hasTelemetry && millis() - lastTelemetryTime < 2000;
  Serial.println("\n=== SYSTEM INFORMATION ===");
  Serial.printf("Chip: %s | Revision: %u | CPU: %u MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
  Serial.printf("Heap frei: %u Bytes | Flash: %u Bytes\n",
                ESP.getFreeHeap(), ESP.getFlashChipSize());
  Serial.printf("LittleFS: %s | %u/%u Bytes | Uptime: %lu s\n",
                totalFlash > 0 ? "bereit" : "nicht verfuegbar",
                usedFlash, totalFlash,
                static_cast<unsigned long>(millis() / 1000));
  Serial.printf("Modell: %s | Kanaele: %u | Empfaenger: %s | Gepairt: %d\n",
                currentConfig.modelName, static_cast<unsigned int>(currentConfig.channelCount),
                linkConnected ? "verbunden" : "offline",
                static_cast<int>(pairedModelCount));
  printDebugStatus();
}

void SerialInterface::printDebugStatus() {
  Serial.printf("Debug-Maske: 0x%08lX | Kanalfilter: %s",
                static_cast<unsigned long>(activeModuleMask.load(std::memory_order_relaxed)),
                targetChannel.load(std::memory_order_relaxed) < 0 ? "aus\n" : "");
  if (targetChannel.load(std::memory_order_relaxed) >= 0) {
    Serial.printf("%d\n", targetChannel.load(std::memory_order_relaxed) + 1);
  }
  for (Module module : modules) {
    Serial.printf("  %-10s %s\n", moduleName(module),
                  isModuleEnabled(module) ? "ON" : "OFF");
  }
}

void SerialInterface::listConfigs() {
  File directory = LittleFS.open(kConfigDirectory);
  if (!directory || !directory.isDirectory()) {
    Serial.println("[SERIAL] Keine gespeicherten Profile.");
    return;
  }
  Serial.println("[SERIAL] Gespeicherte Profile:");
  File file = directory.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      const char* path = file.name();
      const char* baseName = strrchr(path, '/');
      baseName = baseName == NULL ? path : baseName + 1;
      const size_t nameLength = strlen(baseName);
      if (nameLength > 4 && strcmp(baseName + nameLength - 4, ".cfg") == 0) {
        Serial.printf("  %.*s\n", static_cast<int>(nameLength - 4), baseName);
      }
    }
    file.close();
    file = directory.openNextFile();
  }
  directory.close();
}

bool SerialInterface::saveConfig(const char* name) {
  if (!validConfigName(name)) {
    Serial.println("[SERIAL][ERROR] Profilname ungueltig (1-24 Zeichen: A-Z, 0-9, _ oder -).");
    return false;
  }
  if (!LittleFS.exists(kConfigDirectory) && !LittleFS.mkdir(kConfigDirectory)) {
    Serial.println("[SERIAL][ERROR] Profilverzeichnis konnte nicht erstellt werden.");
    return false;
  }

  char path[48];
  snprintf(path, sizeof(path), "%s/%s.cfg", kConfigDirectory, name);
  if (!writeConfigFile(path)) return false;
  Serial.printf("[SERIAL] Aktuelle Diagnosekonfiguration als '%s' gespeichert.\n", name);
  return true;
}

bool SerialInterface::loadConfig(const char* name) {
  if (!validConfigName(name)) {
    Serial.println("[SERIAL][ERROR] Profilname ungueltig (1-24 Zeichen: A-Z, 0-9, _ oder -).");
    return false;
  }
  char path[48];
  snprintf(path, sizeof(path), "%s/%s.cfg", kConfigDirectory, name);
  PersistedConfig config;
  if (!readConfigFile(path, config)) return false;

  activeModuleMask.store(config.moduleMask, std::memory_order_relaxed);
  targetChannel.store(config.targetChannel, std::memory_order_relaxed);
  if (!persistCurrentConfig()) {
    Serial.println("[SERIAL][ERROR] Profil geladen, konnte aber nicht als aktive Konfiguration gesichert werden.");
    return false;
  }
  Serial.printf("[SERIAL] Diagnoseprofil '%s' geladen.\n", name);
  return true;
}

bool SerialInterface::persistCurrentConfig() {
  return writeConfigFile(kActiveConfigPath);
}

bool SerialInterface::validConfigName(const char* name) {
  if (name == NULL) return false;
  const size_t length = strlen(name);
  if (length == 0 || length > 24) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

void SerialInterface::setModuleEnabled(Module module, bool enabled) {
  const uint32_t bit = static_cast<uint32_t>(module);
  const uint32_t currentMask = activeModuleMask.load(std::memory_order_relaxed);
  activeModuleMask.store(enabled ? currentMask | bit : currentMask & ~bit,
                         std::memory_order_relaxed);
  if (!persistCurrentConfig()) {
    Serial.println("[SERIAL][ERROR] Diagnosekonfiguration konnte nicht gespeichert werden.");
  } else {
    Serial.printf("[SERIAL] Modul %s %s.\n", moduleName(module), enabled ? "aktiviert" : "deaktiviert");
  }
}

bool SerialInterface::parseModule(const char* name, Module& module) {
  if (strcmp(name, "SYSTEM") == 0 || strcmp(name, "SYS") == 0) module = Module::System;
  else if (strcmp(name, "MEMORY") == 0 || strcmp(name, "MEM") == 0) module = Module::Memory;
  else if (strcmp(name, "HARDWARE") == 0 || strcmp(name, "HW") == 0) module = Module::Hardware;
  else if (strcmp(name, "CHANNELS") == 0 || strcmp(name, "CHANNEL") == 0 || strcmp(name, "CHAN") == 0) module = Module::Channels;
  else if (strcmp(name, "TELEMETRY") == 0 || strcmp(name, "TEL") == 0) module = Module::Telemetry;
  else if (strcmp(name, "ESPNOW") == 0 || strcmp(name, "ESP-NOW") == 0 || strcmp(name, "RADIO") == 0) module = Module::EspNow;
  else if (strcmp(name, "PAIRING") == 0 || strcmp(name, "PAIR") == 0) module = Module::Pairing;
  else if (strcmp(name, "UI") == 0 || strcmp(name, "LVGL") == 0) module = Module::UI;
  else if (strcmp(name, "BATTERY") == 0 || strcmp(name, "BATT") == 0) module = Module::Battery;
  else if (strcmp(name, "AUDIO") == 0) module = Module::Audio;
  else if (strcmp(name, "MODELS") == 0 || strcmp(name, "MODEL") == 0 || strcmp(name, "CONFIG") == 0) module = Module::Models;
  else if (strcmp(name, "CORE") == 0 || strcmp(name, "TASKS") == 0) module = Module::Core;
  else return false;
  return true;
}
