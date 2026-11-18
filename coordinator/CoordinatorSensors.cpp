#include "CoordinatorSensors.h"

#include <Adafruit_BME680.h>
#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <math.h>

#include "CoordinatorConfig.h"
#include "CoordinatorSensorMath.h"

#if COORDINATOR_METRIC_COUNT != 2
#error "BUILD-IOT-01 requires the temperature and humidity metrics"
#endif
#if COORDINATOR_CONFIGURED_SENSOR_COUNT != 2
#error "Coordinator build requires the BME680 and DHT11 drivers"
#endif
static_assert(COORDINATOR_PHYSICAL_VALIDATION_COMPLETE == 0 ||
                  COORDINATOR_METRIC_COUNT == 2,
              "physical validation requires the complete metric set");

// BUILD-IOT-SENSOR-FAILOVER-04: each physical source owns its own channels.
static_assert(COORDINATOR_BME680_TEMPERATURE_CHANNEL != 0 &&
                  COORDINATOR_BME680_HUMIDITY_CHANNEL != 0 &&
                  COORDINATOR_DHT11_TEMPERATURE_CHANNEL != 0 &&
                  COORDINATOR_DHT11_HUMIDITY_CHANNEL != 0,
              "Frame v1 channels must be non-zero");
static_assert(COORDINATOR_BME680_TEMPERATURE_CHANNEL !=
                      COORDINATOR_BME680_HUMIDITY_CHANNEL &&
                  COORDINATOR_DHT11_TEMPERATURE_CHANNEL !=
                      COORDINATOR_DHT11_HUMIDITY_CHANNEL &&
                  COORDINATOR_BME680_TEMPERATURE_CHANNEL !=
                      COORDINATOR_DHT11_TEMPERATURE_CHANNEL &&
                  COORDINATOR_BME680_TEMPERATURE_CHANNEL !=
                      COORDINATOR_DHT11_HUMIDITY_CHANNEL &&
                  COORDINATOR_BME680_HUMIDITY_CHANNEL !=
                      COORDINATOR_DHT11_TEMPERATURE_CHANNEL &&
                  COORDINATOR_BME680_HUMIDITY_CHANNEL !=
                      COORDINATOR_DHT11_HUMIDITY_CHANNEL,
              "BME680 and DHT11 channels must all be distinct");
static_assert(COORDINATOR_FAILOVER_FAIL_THRESHOLD > 0 &&
                  COORDINATOR_FAILOVER_RECOVERY_THRESHOLD > 0 &&
                  COORDINATOR_BME680_REINIT_INTERVAL_CYCLES > 0,
              "failover thresholds must be > 0");

namespace {

Adafruit_BME680 bme;
DHT dht(COORDINATOR_DHT11_DATA_PIN, DHT11);
bool bme_ready = false;
bool dht_ready = false;
uint8_t bme_address = 0;
uint32_t last_dht_read_ms = 0;
bool dht_has_read = false;

// Failover state (RAM only; not persisted in NVS by design).
coordinator_sensor::FailoverCounters failover_counters = {0, 0, false};
coordinator_sensor::ReinitTimer reinit_timer = {0};
// D-1: initial logic state PRINCIPAL; it does not imply the BME680 is
// available (a failed boot initialization counts in the first cycle).
coordinator_sensor::FailoverState failover_state =
    coordinator_sensor::FailoverState::Principal;
coordinator_sensor::Source last_publish = coordinator_sensor::Source::Bme680;
uint32_t reinit_attempts = 0;

bool readBmePair(int32_t* temperature, int32_t* humidity) {
  if (!bme_ready) {
    Serial.println("[BME680] Read failed (sensor not initialized)");
    return false;
  }
  if (!bme.performReading()) {
    Serial.println("[BME680] Read failed");
    return false;
  }

  // BME680 measurement range: -40..85 C, 0..100 %RH.
  const bool temperature_valid = coordinator_sensor::scaleX100(
      bme.temperature, -4000, 8500, temperature);
  const bool humidity_valid = coordinator_sensor::scaleX100(
      bme.humidity, 0, 10000, humidity);

  Serial.print("[BME680] Temperature=");
  Serial.print(bme.temperature, 2);
  Serial.print(" C validation=");
  Serial.println(temperature_valid ? "valid" : "invalid");
  Serial.print("[BME680] Humidity=");
  Serial.print(bme.humidity, 2);
  Serial.print(" % validation=");
  Serial.println(humidity_valid ? "valid" : "invalid");

  if (!temperature_valid || !humidity_valid) {
    Serial.println("[BME680] Read rejected; fallback required");
    return false;
  }
  return true;
}

bool readDhtPair(int32_t* temperature, int32_t* humidity) {
  if (!dht_ready) {
    Serial.println("[DHT11] Read failed (driver not initialized)");
    return false;
  }
  const uint32_t now = millis();
  if (dht_has_read && static_cast<uint32_t>(now - last_dht_read_ms) <
                           COORDINATOR_DHT_MIN_INTERVAL_MS) {
    Serial.println("[DHT11] Read skipped (minimum interval not elapsed)");
    return false;
  }
  last_dht_read_ms = now;
  dht_has_read = true;
  const float t = dht.readTemperature();
  const float h = dht.readHumidity();
  // DHT11 specified range: 0..50 C, 20..90 %RH.
  const bool temperature_valid =
      coordinator_sensor::scaleX100(t, 0, 5000, temperature);
  const bool humidity_valid =
      coordinator_sensor::scaleX100(h, 2000, 9000, humidity);

  Serial.print("[DHT11] Temperature=");
  Serial.print(t, 2);
  Serial.print(" C validation=");
  Serial.println(temperature_valid ? "valid" : "invalid");
  Serial.print("[DHT11] Humidity=");
  Serial.print(h, 2);
  Serial.print(" % validation=");
  Serial.println(humidity_valid ? "valid" : "invalid");
  if (!temperature_valid || !humidity_valid) {
    Serial.println("[DHT11] Read failed (invalid measurement pair)");
    return false;
  }
  return true;
}

// Writes one pair from a single source. The caller selects the channels of
// that source, so a DHT11 pair can never appear on the BME680 channels.
void setPair(agrotech::FrameFields* fields, uint16_t temperature_channel,
             uint16_t humidity_channel, int32_t temperature,
             int32_t humidity) {
  fields->readings[0].channel = temperature_channel;
  fields->readings[0].metric_code = agrotech::kMetricCodeTemperature;
  fields->readings[0].value = temperature;
  fields->readings[1].channel = humidity_channel;
  fields->readings[1].metric_code = agrotech::kMetricCodeRelativeHumidity;
  fields->readings[1].value = humidity;
  fields->reading_count = 2;
}

// Probes 0x77 then 0x76 and applies the full configuration. Succeeds only if
// begin() and every setting were applied, including the gas heater off:
// Adafruit_BME680::begin() re-enables the heater (320 C / 150 ms) on every
// call, so setGasHeater(0, 0) must follow each successful begin().
// It does not call Wire.begin(); the I2C bus is set up once in setup.
bool bmeInitialize(uint8_t* detected_address) {
  uint8_t address = 0;
  if (bme.begin(0x77)) {
    address = 0x77;
  } else if (bme.begin(0x76)) {
    address = 0x76;
  }
  if (detected_address != nullptr) *detected_address = address;
  if (address == 0) return false;

  const bool temperature_configured =
      bme.setTemperatureOversampling(BME680_OS_8X);
  const bool humidity_configured = bme.setHumidityOversampling(BME680_OS_2X);
  const bool pressure_configured =
      bme.setPressureOversampling(BME680_OS_NONE);
  const bool filter_configured = bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  const bool heater_disabled = bme.setGasHeater(0, 0);
  return temperature_configured && humidity_configured &&
         pressure_configured && filter_configured && heater_disabled;
}

// K rule: while the BME680 is not ready, attempt a reinitialization exactly
// every COORDINATOR_BME680_REINIT_INTERVAL_CYCLES acquisition cycles. A
// successful attempt only allows reads again; recovery still requires R
// consecutive valid readings (state machine).
void maybeReinitializeBme() {
  if (bme_ready) return;
  if (!coordinator_sensor::reinitDue(&reinit_timer,
                                     COORDINATOR_BME680_REINIT_INTERVAL_CYCLES)) {
    return;
  }
  ++reinit_attempts;
  uint8_t address = 0;
  bme_ready = bmeInitialize(&address);
  if (bme_ready) bme_address = address;
  Serial.printf("[BME680] reinit attempt=%lu result=%s addr=0x%02X\n",
                static_cast<unsigned long>(reinit_attempts),
                bme_ready ? "ok" : "fail", static_cast<unsigned>(address));
}

// Readiness after the state machine decided the cycle:
// - just became degraded with the sensor still marked ready: mark it not
//   ready and attempt a reinitialization in the next cycle;
// - already degraded and the BME680 failed again while marked ready (for
//   example right after a successful reinit with invalid readings): mark it
//   not ready and wait a full K interval before the next attempt.
// If it was already not ready, the running K interval is kept unchanged.
void updateBmeReadiness(const coordinator_sensor::FailoverDecision& decision,
                        bool bme_valid) {
  if (!bme_ready || bme_valid) return;
  if (decision.entered_degraded) {
    bme_ready = false;
    coordinator_sensor::reinitNextCycle(&reinit_timer);
  } else if (failover_counters.degraded) {
    bme_ready = false;
    coordinator_sensor::reinitAfterAttempt(
        &reinit_timer, COORDINATOR_BME680_REINIT_INTERVAL_CYCLES);
  }
}

void logTransition(const coordinator_sensor::FailoverDecision& decision) {
  if (decision.state == failover_state && decision.publish == last_publish) {
    return;
  }
  Serial.printf("[SOURCE] %s->%s reason=%s fail=%u ok=%u\n",
                coordinator_sensor::failoverStateName(failover_state),
                coordinator_sensor::failoverStateName(decision.state),
                coordinator_sensor::failoverTransitionReason(
                    failover_state, last_publish, decision),
                static_cast<unsigned>(failover_counters.fail),
                static_cast<unsigned>(failover_counters.ok));
}

}  // namespace

void initializeSensors() {
  Serial.print("[I2C] Initializing SDA=");
  Serial.print(COORDINATOR_BME680_SDA_PIN);
  Serial.print(" SCL=");
  Serial.println(COORDINATOR_BME680_SCL_PIN);

  coordinator_sensor::initFailoverCounters(&failover_counters);
  failover_state = coordinator_sensor::FailoverState::Principal;
  last_publish = coordinator_sensor::Source::Bme680;
  reinit_attempts = 0;

  const bool i2c_ready =
      Wire.begin(COORDINATOR_BME680_SDA_PIN, COORDINATOR_BME680_SCL_PIN);
  bme_ready = false;
  bme_address = 0;
  if (i2c_ready) {
    Serial.println("[I2C] Initialized");
    uint8_t address = 0;
    bme_ready = bmeInitialize(&address);
    if (address != 0) {
      bme_address = address;
      Serial.print("[BME680] Detected address=0x");
      Serial.println(bme_address, HEX);
      if (bme_ready) {
        Serial.println("[BME680] Initialization successful (gas heater disabled)");
      } else {
        Serial.println("[BME680] Initialization failed (configuration error)");
      }
    } else {
      Serial.println("[BME680] Initialization failed (not detected at 0x77 or 0x76)");
    }
  } else {
    Serial.println("[I2C] Initialization failed");
    Serial.println("[BME680] Initialization skipped (I2C unavailable)");
  }
  // The setup attempt counts as cycle 0: if the sensor is not ready, the
  // next attempt happens at acquisition cycle K.
  coordinator_sensor::reinitAfterAttempt(
      &reinit_timer, COORDINATOR_BME680_REINIT_INTERVAL_CYCLES);

  dht.begin();
  dht_ready = true;
  Serial.println("[DHT11] Driver initialized on GPIO 4; availability checked on read");
}

coordinator::SensorSource readConfiguredSensors(agrotech::FrameFields* fields) {
  if (fields == nullptr) return coordinator::SensorSource::None;
  coordinator_sensor::clearReadings(fields);

  maybeReinitializeBme();

  int32_t bme_temperature = 0;
  int32_t bme_humidity = 0;
  int32_t dht_temperature = 0;
  int32_t dht_humidity = 0;
  const bool bme_valid = readBmePair(&bme_temperature, &bme_humidity);

  bool dht_valid = false;
  if (coordinator_sensor::failoverNeedsDht(failover_counters, bme_valid)) {
    Serial.println(bme_valid ? "[DHT11] Reading backup during recovery"
                             : "[DHT11] Attempting fallback");
    dht_valid = readDhtPair(&dht_temperature, &dht_humidity);
  }

  const coordinator_sensor::FailoverDecision decision =
      coordinator_sensor::failoverStep(
          &failover_counters, bme_valid, dht_valid,
          COORDINATOR_FAILOVER_FAIL_THRESHOLD,
          COORDINATOR_FAILOVER_RECOVERY_THRESHOLD);
  updateBmeReadiness(decision, bme_valid);
  logTransition(decision);
  failover_state = decision.state;
  last_publish = decision.publish;

  if (decision.publish == coordinator_sensor::Source::Bme680) {
    setPair(fields, COORDINATOR_BME680_TEMPERATURE_CHANNEL,
            COORDINATOR_BME680_HUMIDITY_CHANNEL, bme_temperature,
            bme_humidity);
    Serial.println("[SENSOR] Source=BME680");
    return coordinator::SensorSource::Bme680;
  }
  if (decision.publish == coordinator_sensor::Source::Dht11) {
    setPair(fields, COORDINATOR_DHT11_TEMPERATURE_CHANNEL,
            COORDINATOR_DHT11_HUMIDITY_CHANNEL, dht_temperature,
            dht_humidity);
    Serial.println("[SENSOR] Source=DHT11");
    return coordinator::SensorSource::Dht11;
  }
  Serial.println("[SENSOR] No valid measurement");
  Serial.println("[FRAME] Not generated");
  return coordinator::SensorSource::None;
}

coordinator_sensor::FailoverState currentFailoverState() {
  return failover_state;
}
