#ifndef AGROTECH_GATEWAY_CONFIG_H
#define AGROTECH_GATEWAY_CONFIG_H

#include <stdint.h>
#include <AgrotechRadioConfig.h>

#if __has_include("GatewayConfig.local.h")
#include "GatewayConfig.local.h"
#endif

// Gateway UID and MQTT credentials are provisioned into ESP32 NVS locally.
// Never commit per-device credentials; see docs/SECURITY-PROVISIONING.md.

// MQTT broker and trust anchor are deployment configuration, not device secrets.
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 0
#endif
#ifndef MQTT_CA_CERT
#define MQTT_CA_CERT ""
#endif

// NTP. Used only to timestamp lora_rx_at and set clock_synced.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
static const uint32_t GATEWAY_WIFI_RETRY_MS = 10000;

// MQTT reconnection backoff (MQTT-RESILIENCE-04). Replaces the former fixed
// GATEWAY_MQTT_RETRY_MS (5 s). Network-class failures (DNS, TCP, TLS
// negotiation, timeouts, lost link, CONNACK "server unavailable") retry from
// INITIAL, doubling up to MAX. Broker rejections (CONNACK 1/2/4/5) and
// certificate verification failures use the slower AUTH schedule, so a wrong
// or revoked token never causes a fast reconnect loop. Every wait is
// jittered in [delay/2, delay]. Both schedules reset once a connection has
// stayed up for GATEWAY_MQTT_STABLE_MS. Override in GatewayConfig.local.h.
#ifndef GATEWAY_MQTT_BACKOFF_INITIAL_MS
#define GATEWAY_MQTT_BACKOFF_INITIAL_MS 2000
#endif
#ifndef GATEWAY_MQTT_BACKOFF_MAX_MS
#define GATEWAY_MQTT_BACKOFF_MAX_MS 120000
#endif
#ifndef GATEWAY_MQTT_AUTH_BACKOFF_INITIAL_MS
#define GATEWAY_MQTT_AUTH_BACKOFF_INITIAL_MS 300000
#endif
#ifndef GATEWAY_MQTT_AUTH_BACKOFF_MAX_MS
#define GATEWAY_MQTT_AUTH_BACKOFF_MAX_MS 3600000
#endif
#ifndef GATEWAY_MQTT_STABLE_MS
#define GATEWAY_MQTT_STABLE_MS 60000
#endif
// Upper bound for one TLS handshake (the ESP32 core default is 120 s). It
// only bounds the wait; certificate verification is unchanged.
#ifndef GATEWAY_TLS_HANDSHAKE_TIMEOUT_S
#define GATEWAY_TLS_HANDSHAKE_TIMEOUT_S 30
#endif

// Store-and-forward window for LoRa frames when MQTT is down. Since
// MQTT-QUEUE-PERSISTENCE-05 it is the durable frame store (GatewayFrameStore):
// 16 live frames in total, including the one being published, persisted on
// flash and recovered after a reboot. The capacity is fixed by the store
// format (gateway_frame_log::kLiveCapacity); this macro must stay 16.
#ifndef GATEWAY_MQTT_QUEUE_DEPTH
#define GATEWAY_MQTT_QUEUE_DEPTH 16
#endif

// Durable frame store (MQTT-QUEUE-PERSISTENCE-05). Uses the EXISTING data
// partition of the default ESP32 Dev Module scheme (label "spiffs", subtype
// spiffs, 0x160000 bytes); the partition table is not changed. The store
// initializes the partition only if every byte is erased (0xFF) and never
// erases unknown data. Diagnostic builds use a volatile RAM store instead.
#ifndef GATEWAY_FRAME_STORE_PARTITION_LABEL
#define GATEWAY_FRAME_STORE_PARTITION_LABEL "spiffs"
#endif
#ifndef GATEWAY_FRAME_STORE_PARTITION_SIZE
#define GATEWAY_FRAME_STORE_PARTITION_SIZE 0x160000UL
#endif
// Longest wait for the store mutex (it may be held during one sector erase
// by the MQTT task). On timeout loop() rejects the frame instead of waiting.
#ifndef GATEWAY_FRAME_STORE_LOCK_TIMEOUT_MS
#define GATEWAY_FRAME_STORE_LOCK_TIMEOUT_MS 500
#endif
// TLS + lwmqtt + payload construction run in this task, not in loop().
#ifndef GATEWAY_MQTT_TASK_STACK_BYTES
#define GATEWAY_MQTT_TASK_STACK_BYTES 16384
#endif

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

#endif
