/**
 * @file hardware_io.cpp
 * @version 1.1.1
 * @brief Implementierung der Hardware-Abstraktion mit I2C-Caching und SerialInterface (v1.1.1).
 * @Version_info v1.1.1: Ersetzung von #define DEBUG durch das dynamische SerialInterface.
 */

#include "hardware_io.h"
#include "SerialInterface.h"
#include <Adafruit_ADS1X15.h>
#include <Arduino.h>
#include <Wire.h>
#include "LittleFS.h"

SystemCalibrationConfig sysCalibration;

const HardwareInputDef AVAILABLE_HARDWARE_INPUTS[NUM_HARDWARE_INPUTS] = {
  { 0, "Kein Geber", 0x00, 0 },
  { 1, "Joystick L X", 0x48, 0 },  
  { 2, "Joystick L Y", 0x48, 1 },  
  { 3, "Joystick R X", 0x48, 2 },
  { 4, "Joystick R Y", 0x48, 3 },
  { 5, "Schalter A", 0x00, 0 },
  { 6, "Schalter B", 0x00, 0 },
  { 7, "Taster C", 0x00, 0 },
  { 8, "Encoder D", 0x00, 0 }
};

Adafruit_ADS1115 ads1;

// Interner Cache fuer die 4 analogen Achsen zur Entlastung des I2C-Busses
static int16_t cached_raw_axes[4] = {8759, 8759, 8759, 8759}; // Start mit Mittelstellung
static uint8_t current_cache_channel = 0;

/**
 * @brief Initialisiert den I2C-Bus (400 kHz) und den ADS1115.
 */
void init_hardware_io(void) {
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);  // 400 kHz I2C-Takt (Fast Mode)
  Wire.setTimeOut(2);                           // 2 ms Timeout
  
  if (!ads1.begin(0x48, &Wire)) {
    SerialInterface::printError("[HW][ERROR] ADS1115 (0x48) nicht gefunden!");
  } else {
    ads1.setGain(GAIN_TWOTHIRDS);
    ads1.setDataRate(RATE_ADS1115_860SPS); // Maximale Konvertierungsgeschwindigkeit 860SPS. Alternativ 475 SPS oder 330 SPS, 128SPS
    SerialInterface::printDebug(SerialInterface::Module::Hardware,
                                "ADS1115 (0x48) erfolgreich gestartet.");
  }
  
  load_calibration();
}

/**
 * @brief Aktualisiert jeweils einen Achsenkanal im Round-Robin Verfahren, um den I2C-Bus nicht zu blockieren.
 */
void update_hardware_cache(void) {
  static unsigned long last_cache_update = 0;
  if (millis() - last_cache_update >= 5) { // Alle 5ms einen Kanal abfragen
    last_cache_update = millis();
    
    // ADS1115 Einzelkanal-Abfrage (nicht-blockierend verteilt)
    cached_raw_axes[current_cache_channel] = ads1.readADC_SingleEnded(current_cache_channel);
    
    current_cache_channel = (current_cache_channel + 1) % 4;
  }
}

/**
 * @brief Gibt den gecachten Rohwert der analogen Achse sofort zurueck (ohne I2C-Blockade).
 */
int16_t read_hardware_raw(uint8_t hw_id) {
  if (hw_id == 0 || hw_id >= NUM_HARDWARE_INPUTS) {
    return 0;  
  }

  const HardwareInputDef& input = AVAILABLE_HARDWARE_INPUTS[hw_id];

  if (input.i2cAddress == 0x48) {
    if (input.adsChannel < 4) {
      return cached_raw_axes[input.adsChannel];
    }
  }

  return 0;
}

const char* get_hardware_label(uint8_t hw_id) {
  if (hw_id == 0 || hw_id >= NUM_HARDWARE_INPUTS) {
    return "Kein Geber";
  }
  return AVAILABLE_HARDWARE_INPUTS[hw_id].label;
}

// --- KALIBRIERUNGS- UND SYSTEM-METHODEN ---

void reset_calibration_to_default(void) {
  sysCalibration.magicMarker = 0xCA11BEF2;
  for (int i = 0; i < 4; i++) {
    sysCalibration.axes[i].rawMin = 6560;
    sysCalibration.axes[i].rawCenter = 8759;
    sysCalibration.axes[i].rawMax = 10980;
  }
  sysCalibration.displayBrightness = 80; // Standard Helligkeit: 80%
  sysCalibration.autoDimEnabled = 0;     // Standard Auto-Dimming: Aus (0)
}

void save_calibration(void) {
  fs::File file = LittleFS.open("/calibration.bin", FILE_WRITE);
  if (!file) return;
  file.write((const uint8_t*)&sysCalibration, sizeof(SystemCalibrationConfig));
  file.close();
}

bool load_calibration(void) {
  if (!LittleFS.exists("/calibration.bin")) {
    reset_calibration_to_default();
    save_calibration();
    return false;
  }
  
  fs::File file = LittleFS.open("/calibration.bin", FILE_READ);
  if (!file) {
    reset_calibration_to_default();
    return false;
  }

  if (file.size() != sizeof(SystemCalibrationConfig)) {
    file.close();
    reset_calibration_to_default();
    save_calibration();
    return true;
  }

  file.read((uint8_t*)&sysCalibration, sizeof(SystemCalibrationConfig));
  file.close();

  if (sysCalibration.magicMarker != 0xCA11BEF2) {
    reset_calibration_to_default();
    save_calibration();
  }
  return true;
}

uint16_t read_hardware_calibrated_pwm(uint8_t hw_id, int16_t raw_val, uint8_t deadband_pct) {
  if (hw_id == 0 || hw_id > 4) return 1500; 
  
  const AnalogCalibration& cal = sysCalibration.axes[hw_id - 1];
  int center = cal.rawCenter;
  int min_v = cal.rawMin;
  int max_v = cal.rawMax;

  int max_delta_up = max_v - center;
  int max_delta_down = center - min_v;
  if (max_delta_up < 10) max_delta_up = 10;
  if (max_delta_down < 10) max_delta_down = 10;

  int dynamic_deadband = (int)((float)((max_delta_up > max_delta_down ? max_delta_up : max_delta_down)) * ((float)deadband_pct / 100.0));
  if (dynamic_deadband < 40) dynamic_deadband = 40;

  float pwm_val = 1500.0f;
  if (raw_val > center + dynamic_deadband) {
    float fraction = (float)(raw_val - (center + dynamic_deadband)) / (float)(max_delta_up - dynamic_deadband);
    if (fraction > 1.0f) fraction = 1.0f;
    pwm_val = 1500.0f + (fraction * 500.0f);
  } else if (raw_val < center - dynamic_deadband) {
    float fraction = (float)(center - dynamic_deadband - raw_val) / (float)(max_delta_down - dynamic_deadband);
    if (fraction > 1.0f) fraction = 1.0f;
    pwm_val = 1500.0f - (fraction * 500.0f);
  }

  if (pwm_val < 1000.0f) pwm_val = 1000.0f;
  if (pwm_val > 2000.0f) pwm_val = 2000.0f;
  return (uint16_t)round(pwm_val);
}

int16_t read_hardware_calibrated_percent(uint8_t hw_id, int16_t raw_val, uint8_t deadband_pct) {
  uint16_t pwm = read_hardware_calibrated_pwm(hw_id, raw_val, deadband_pct);
  return (int16_t)((int32_t)(pwm - 1500) / 5);
}

Adafruit_MCP23X17 mcp;
Adafruit_MAX17048 maxlipo;
static unsigned long boot_time_ms = 0;

void init_hardware_extensions(void) {
  boot_time_ms = millis();
  mcp.begin_I2C(0x20, &Wire);
  maxlipo.begin(&Wire);
}

static const uint8_t BACKLIGHT_LEDC_CHANNEL = 4;

void init_backlight(void) {
  pinMode(BACKLIGHT_PIN, OUTPUT);
  ledcSetup(BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_FREQ, BACKLIGHT_RES_BITS);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_LEDC_CHANNEL);
  set_display_brightness(sysCalibration.displayBrightness);
}

void set_display_brightness(uint8_t percent) {
  if (percent < 10) percent = 10;
  if (percent > 100) percent = 100;
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, (255 * percent) / 100);
}

void mcp_pin_mode(uint8_t pin, uint8_t mode) { mcp.pinMode(pin, mode); }
void mcp_digital_write(uint8_t pin, uint8_t val) { mcp.digitalWrite(pin, val); }
bool mcp_digital_read(uint8_t pin) { return mcp.digitalRead(pin); }

float get_battery_voltage(void) { float v = maxlipo.cellVoltage(); return isnan(v) ? 0.0f : v; }
float get_battery_percentage(void) { float p = maxlipo.cellPercent(); return isnan(p) ? 0.0f : p; }
uint32_t get_uptime_seconds(void) { return (millis() - boot_time_ms) / 1000; }
bool is_battery_charging(void) { float rate = maxlipo.chargeRate(); return isnan(rate) ? false : (rate > 0.0f); }
uint32_t get_estimated_charging_seconds(void) { return 3600; }
uint32_t get_estimated_remaining_seconds(void) { return 7200; }