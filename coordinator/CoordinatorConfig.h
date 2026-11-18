#ifndef AGROTECH_COORDINATOR_CONFIG_H
#define AGROTECH_COORDINATOR_CONFIG_H

#include <stdint.h>
#include <AgrotechRadioConfig.h>

// Identity and HMAC key are provisioned into ESP32 NVS at installation time.
// Never commit per-device credentials; see docs/SECURITY-PROVISIONING.md.

// Shared historical radio pins; physical continuity/module variants remain
// subject to confirmation before enabling the transport.
static const int LORA_PIN_SCK = AGROTECH_LORA_PIN_SCK;
static const int LORA_PIN_MISO = AGROTECH_LORA_PIN_MISO;
static const int LORA_PIN_MOSI = AGROTECH_LORA_PIN_MOSI;
static const int LORA_PIN_SS = AGROTECH_LORA_PIN_NSS;
static const int LORA_PIN_RESET = AGROTECH_LORA_PIN_RESET;
static const int LORA_PIN_DIO0 = AGROTECH_LORA_PIN_DIO0;

// Shared legacy RF profile. Unset values keep radio initialization blocked.
#define LORA_FREQUENCY_HZ AGROTECH_LORA_FREQUENCY_HZ
#define LORA_SPREADING_FACTOR AGROTECH_LORA_SPREADING_FACTOR
#define LORA_BANDWIDTH_HZ AGROTECH_LORA_BANDWIDTH_HZ
#define LORA_CODING_RATE_DENOMINATOR AGROTECH_LORA_CODING_RATE_DENOMINATOR
#define LORA_TX_POWER_DBM AGROTECH_LORA_TX_POWER_DBM
#define LORA_SYNC_WORD AGROTECH_LORA_SYNC_WORD
#define LORA_PREAMBLE_LENGTH AGROTECH_LORA_PREAMBLE_LENGTH
#define LORA_USE_CRC AGROTECH_LORA_USE_CRC

// Confirmed historical wiring for the coordinator hardware.
static const int COORDINATOR_BME680_SDA_PIN = 21;
static const int COORDINATOR_BME680_SCL_PIN = 22;
static const int COORDINATOR_DHT11_DATA_PIN = 4;
static const int COORDINATOR_LED_PIN = 25;
// Confirmed LED wiring is active HIGH: GPIO25 -> resistor -> LED anode -> GND.
static const uint8_t COORDINATOR_LED_ON_LEVEL = 1;   // Arduino HIGH
static const uint8_t COORDINATOR_LED_OFF_LEVEL = 0;  // Arduino LOW
// Frame v1 metric cardinality: one temperature plus one relative-humidity
// reading per frame. This is a protocol property and does not describe how many
// physical sensors are wired.
#define COORDINATOR_METRIC_COUNT 2

// Physical sensor drivers instantiated by this build: BME680 (primary) and
// DHT11 (backup).
#define COORDINATOR_CONFIGURED_SENSOR_COUNT 2

// Physical bring-up sign-off. It must stay 0 until the hardware validation is
// complete; it is independent of the metric and sensor counts above and is the
// only flag that may declare the firmware physically validated.
#define COORDINATOR_PHYSICAL_VALIDATION_COMPLETE 0

// Operational settings.
static const uint32_t COORDINATOR_TX_INTERVAL_MS = 60000;
static const uint32_t COORDINATOR_SEQ_CHECKPOINT_EVERY = 16;
static const uint32_t COORDINATOR_DHT_MIN_INTERVAL_MS = 2000;

// BUILD-IOT-SENSOR-FAILOVER-04: the frame channel identifies the physical
// source. A frame always carries one pair from a single sensor; the DHT11 is
// never sent on the BME680 channels and vice versa.
static const uint16_t COORDINATOR_BME680_TEMPERATURE_CHANNEL = 1;
static const uint16_t COORDINATOR_BME680_HUMIDITY_CHANNEL = 2;
static const uint16_t COORDINATOR_DHT11_TEMPERATURE_CHANNEL = 3;
static const uint16_t COORDINATOR_DHT11_HUMIDITY_CHANNEL = 4;

// Failover thresholds, counted in acquisition cycles (one cycle = one
// COORDINATOR_TX_INTERVAL_MS period).
static const uint8_t COORDINATOR_FAILOVER_FAIL_THRESHOLD = 3;      // F
static const uint8_t COORDINATOR_FAILOVER_RECOVERY_THRESHOLD = 3;  // R
static const uint8_t COORDINATOR_BME680_REINIT_INTERVAL_CYCLES = 5;  // K

#endif
