/**
 * @file SerialInterface.h
 * @brief Runtime-configurable serial diagnostics and command interface.
 */

#ifndef SERIAL_INTERFACE_H
#define SERIAL_INTERFACE_H

#include <Arduino.h>
#include <stdint.h>
#include "config_types.h"

class SerialInterface {
public:
  /** Diagnostic areas whose output can be toggled and saved in a profile. */
  enum class Module : uint32_t {
    System    = 1u << 0,
    Memory    = 1u << 1,
    Hardware  = 1u << 2,
    Channels  = 1u << 3,
    Telemetry = 1u << 4,
    EspNow    = 1u << 5,
    Pairing   = 1u << 6,
    UI        = 1u << 7,
    Battery   = 1u << 8,
    Audio     = 1u << 9,
    Models    = 1u << 10,
    Core      = 1u << 11,
    All       = (1u << 12) - 1
  };

  /** Loads the active LittleFS profile and prints command help. Call after mounting LittleFS. */
  static void begin();

  /** Processes available serial input without waiting for a complete command. */
  static void update();

  /** Prints a message when the System module is enabled. */
  static void printSystem(const char* format, ...);

  /** Prints an error message regardless of the current debug-module settings. */
  static void printError(const char* format, ...);

  /** Prints a formatted message when the System module is enabled. */
  static void printDebug(const char* format, ...);

  /** Prints a formatted message when the specified diagnostic module is enabled. */
  static void printDebug(Module module, const char* format, ...);

  /** Prints a telemetry message when the Telemetry module is enabled. */
  static void printTelemetryLog(const char* format, ...);

  /** Returns true if at least one diagnostic module is enabled. */
  static bool isGlobalDebugEnabled();

  /** Returns whether telemetry diagnostics are enabled. */
  static bool isTelemetryDebugEnabled();

  /** Returns whether diagnostics for the supplied module are enabled. */
  static bool isModuleEnabled(Module module);

  /** Returns whether a zero-based channel should produce channel diagnostics. */
  static bool isChannelEnabled(int channelIndex);

  /** Returns the selected channel filter (zero-based), or -1 when not selected. */
  static int getTargetChannel();

private:
  static void processCommand(char* command);
  static void printHelp();
  static void printModelInfo();
  static void printSystemInfo();
  static void printDebugStatus();
  static void listConfigs();
  static bool saveConfig(const char* name);
  static bool loadConfig(const char* name);
  static bool persistCurrentConfig();
  static bool validConfigName(const char* name);
  static void setModuleEnabled(Module module, bool enabled);
  static bool parseModule(const char* name, Module& module);
};

#endif // SERIAL_INTERFACE_H
