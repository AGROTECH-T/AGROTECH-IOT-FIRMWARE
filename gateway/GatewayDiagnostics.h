#ifndef AGROTECH_GATEWAY_DIAGNOSTICS_H
#define AGROTECH_GATEWAY_DIAGNOSTICS_H

// ===========================================================================
// BUILD-IOT-06 - Physical MQTT diagnostic mode (OFF by default).
//
// Purpose: prove Wi-Fi + TLS + individual credentials + QoS 1 PUBACK + backend
// durable receipt from the real ESP32, WITHOUT the LoRa radio. A known
// official Frame v1 vector is injected into the normal store-and-forward
// queue, so the production builder, queue, MQTT task and publish path are the
// ones exercised.
//
// Enable ONLY in the ignored gateway/GatewayConfig.local.h of a staging build:
//   #define GATEWAY_MQTT_DIAG_ENABLED 1
// A diagnostic build is never a production build: it refuses to compile if
// the LoRa or production MQTT flags are enabled, never starts the radio, and
// publishes only when the operator types "DIAG PUBLISH" on the serial console.
// See docs/BUILD-IOT-06-MQTT-DIAGNOSTIC.md.
// ===========================================================================

#include <stddef.h>
#include <stdint.h>

#include <AgrotechProtocol.h>

#ifndef GATEWAY_MQTT_DIAG_ENABLED
#define GATEWAY_MQTT_DIAG_ENABLED 0
#endif

// Hard cap on injected publishes per boot (guards against a stuck console).
#ifndef GATEWAY_MQTT_DIAG_MAX_PUBLISHES
#define GATEWAY_MQTT_DIAG_MAX_PUBLISHES 10
#endif

namespace gateway_diag {

static constexpr bool kEnabled = GATEWAY_MQTT_DIAG_ENABLED != 0;
static_assert(GATEWAY_MQTT_DIAG_MAX_PUBLISHES > 0 &&
                  GATEWAY_MQTT_DIAG_MAX_PUBLISHES <= 100,
              "GATEWAY_MQTT_DIAG_MAX_PUBLISHES must be between 1 and 100");

// Official backend reference vector (test_vectors/official_vector.json,
// source: AGROTECH-T BACKEND apps/iot/tests/lora_vectors.py).
// coordinator CO-7K3M9QXT2A, key_version 1, boot 7, seq 120, 2 readings.
// Checked byte-for-byte by tools/test_gateway_diag_vector.py.
static constexpr size_t kFrameLength = 67;
static const uint8_t kFrame[kFrameLength] = {
    0x01, 0x43, 0x4F, 0x2D, 0x37, 0x4B, 0x33, 0x4D, 0x39, 0x51, 0x58, 0x54,
    0x32, 0x41, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x78,
    0x02, 0x00, 0x01, 0x01, 0x09, 0xE6, 0x00, 0x02, 0x02, 0x18, 0xB5, 0xDD,
    0xBA, 0xB4, 0x81, 0xE3, 0xB4, 0xF7, 0x1D, 0xCC, 0x93, 0x45, 0x2B, 0x1D,
    0x09, 0x43, 0xAD, 0x24, 0x16, 0x3C, 0x59, 0xD6, 0xA3, 0xC2, 0xE6, 0xF2,
    0x7A, 0xD3, 0xEB, 0x6B, 0xAB, 0x1A, 0xF5,
};

// PUBLIC test key of the vector (not a credential). Used only to self-check
// the embedded frame's HMAC at boot, so a corrupted build never publishes.
static const uint8_t kPublicTestKey[agrotech::kHmacKeyBytes] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

// Deterministic transport metadata: no radio exists in this mode, so RSSI/SNR
// are the vector's reference values and the frame is sent with
// lora_rx_at=null / clock_synced=false. Identical publishes therefore produce
// byte-identical JSON (same backend payload_sha256 / transport key).
// Heap snapshot type lives here (not in the .ino) so Arduino's generated
// function prototypes can see it.
struct HeapSnapshot {
  uint32_t free_bytes;
  uint32_t min_free_bytes;
  uint32_t largest_block;
};

static constexpr int kRssi = -87;
static constexpr float kSnr = 7.5f;

}  // namespace gateway_diag

#endif
