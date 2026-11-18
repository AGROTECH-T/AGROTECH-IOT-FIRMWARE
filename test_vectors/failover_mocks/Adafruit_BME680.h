// Host test double for Adafruit BME680 2.0.6 (BUILD-IOT-SENSOR-FAILOVER-05).
// Mirrors the confirmed library behaviour that matters here: begin() probes a
// single address and (with initSettings=true) re-enables the gas heater
// (320 C / 150 ms); setGasHeater(0, 0) disables it.
#ifndef AGROTECH_FAILOVER_MOCK_BME680_H
#define AGROTECH_FAILOVER_MOCK_BME680_H

#include <stdint.h>

#define BME680_OS_NONE 0
#define BME680_OS_2X 2
#define BME680_OS_8X 4
#define BME680_FILTER_SIZE_3 2

struct FakeBmeState {
  uint8_t present_address;  // 0: not connected
  bool config_ok;
  bool perform_ok;
  float next_temperature;
  float next_humidity;
  int begin_calls;
  int heater_off_calls;
  bool heater_enabled;
};

extern FakeBmeState g_fake_bme;

class Adafruit_BME680 {
 public:
  float temperature = 0;
  float humidity = 0;
  float pressure = 0;
  uint32_t gas_resistance = 0;

  bool begin(uint8_t address = 0x77, bool init_settings = true) {
    ++g_fake_bme.begin_calls;
    if (g_fake_bme.present_address == 0 || address != g_fake_bme.present_address)
      return false;
    if (init_settings) g_fake_bme.heater_enabled = true;  // library default
    return true;
  }
  bool setTemperatureOversampling(uint8_t) { return g_fake_bme.config_ok; }
  bool setHumidityOversampling(uint8_t) { return g_fake_bme.config_ok; }
  bool setPressureOversampling(uint8_t) { return g_fake_bme.config_ok; }
  bool setIIRFilterSize(uint8_t) { return g_fake_bme.config_ok; }
  bool setGasHeater(uint16_t heater_temp, uint16_t heater_time) {
    if (heater_temp == 0 || heater_time == 0) {
      g_fake_bme.heater_enabled = false;
      ++g_fake_bme.heater_off_calls;
    } else {
      g_fake_bme.heater_enabled = true;
    }
    return true;
  }
  bool performReading() {
    if (g_fake_bme.present_address == 0 || !g_fake_bme.perform_ok) return false;
    temperature = g_fake_bme.next_temperature;
    humidity = g_fake_bme.next_humidity;
    return true;
  }
};

#endif
