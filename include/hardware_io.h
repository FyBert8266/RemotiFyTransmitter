/**
 * @file hardware_io.h
 * @version 1.1.0
 * @brief Header-Datei fuer Hardware-Abstraktion mit optimiertem Sensor-Caching (v1.1.0).
 * @Version_info v1.1.0: Ergaenzung von update_hardware_cache zur Entlastung des I2C-Busses und Steigerung der Loop-Performance.
 */

#ifndef HARDWARE_IO_H
#define HARDWARE_IO_H

#include <Arduino.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_MCP23X17.h>
#include "Adafruit_MAX1704X.h"
#include "config_types.h"

// CYD Standard-Pins für I2C
#define I2C_SDA_PIN 32
#define I2C_SCL_PIN 25

// Backlight / PWM Display-Helligkeit (IO27)
#define BACKLIGHT_PIN 27
#define BACKLIGHT_FREQ 5000
#define BACKLIGHT_RES_BITS 8

// Struktur für die Hardware-Eingabeelemente
struct HardwareInputDef {
  uint8_t id;          // Eindeutige ID
  char label[16];      // Bezeichnung
  uint8_t i2cAddress;  // I2C Adresse
  uint8_t adsChannel;  // ADS Kanal
};

#define NUM_HARDWARE_INPUTS 9

// Externe Deklarationen
extern const HardwareInputDef AVAILABLE_HARDWARE_INPUTS[NUM_HARDWARE_INPUTS];
extern SystemCalibrationConfig sysCalibration;

extern Adafruit_MCP23X17 mcp;
extern Adafruit_MAX17048 maxlipo;

// Funktionsprototypen
void init_hardware_io(void);
void init_hardware_extensions(void);
void update_hardware_cache(void); // NEU: Asynchroner Cache-Update
int16_t read_hardware_raw(uint8_t hw_id);
const char* get_hardware_label(uint8_t hw_id);

// Kalibrierungs-Funktionen
void init_calibration(void);
void save_calibration(void);
bool load_calibration(void);
void reset_calibration_to_default(void);
uint16_t read_hardware_calibrated_pwm(uint8_t hw_id, int16_t raw_val, uint8_t deadband_pct);
int16_t read_hardware_calibrated_percent(uint8_t hw_id, int16_t raw_val, uint8_t deadband_pct);

// MCP23017 digitale I/O Wrapper
void mcp_pin_mode(uint8_t pin, uint8_t mode);
void mcp_digital_write(uint8_t pin, uint8_t val);
bool mcp_digital_read(uint8_t pin);

// MAX17048 Batterie-Metriken
float get_battery_voltage(void);
float get_battery_percentage(void);
uint32_t get_uptime_seconds(void);
uint32_t get_estimated_remaining_seconds(void);
bool is_battery_charging(void);
uint32_t get_estimated_charging_seconds(void);

// Backlight / PWM Display-Helligkeit
void init_backlight(void);
void set_display_brightness(uint8_t percent);

#endif  // HARDWARE_IO_H