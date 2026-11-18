#ifndef AGROTECH_GATEWAY_FRAME_STORE_H
#define AGROTECH_GATEWAY_FRAME_STORE_H

// ===========================================================================
// MQTT-QUEUE-PERSISTENCE-05 - durable frame store (ESP32 adapter).
//
// Single source of truth for the gateway's store-and-forward queue: up to
// gateway_frame_log::kLiveCapacity (16) live frames, including the one being
// published. Wraps GatewayFrameLog over the existing "spiffs" data partition
// and serializes every access with one FreeRTOS mutex.
//
// Ownership:
//   * loop()     -> store()                       (FRAME append, never erases)
//   * mqttTask() -> peekHead(), freeze(), acknowledge(), maintain()
//                   (maintain() is the only caller that erases sectors)
//   * the mutex is never held across network calls (callers do not hold it;
//     each function takes and releases it internally).
//
// Diagnostic builds (GATEWAY_MQTT_DIAG_ENABLED) use an explicit volatile RAM
// region and never look up, read or write the flash partition.
// ===========================================================================

#include <stddef.h>
#include <stdint.h>

#include "GatewayFrameLog.h"

namespace gateway_frame_store {

enum class Mode : uint8_t {
  Disabled,  // begin() not called (MQTT path disabled): nothing accepted
  Durable,   // flash partition
  Volatile,  // diagnostic builds only: RAM, lost on reboot, by design
};

struct Counters {
  uint32_t accepted;              // FRAME written and verified
  uint32_t rejected_full;         // 16 live frames already stored
  uint32_t rejected_unavailable;  // store not ready (partition/state)
  uint32_t rejected_write;        // FRAME write or verify failed / no space
  uint32_t lock_timeouts;
  uint32_t freeze_failures;
  uint32_t ack_failures;
};

// Opens (and, only if the region is completely erased, initializes) the
// store. Returns ready(). Must run before the MQTT task starts.
bool begin(bool diagnostic_build);

bool ready();
Mode mode();
// Fixed text describing the current state or the reason it is unavailable.
const char* stateText();

// loop(): persists one received frame. Ok only after the FRAME record has
// been written and verified; any other status means "not accepted".
gateway_frame_log::Status store(const uint8_t* frame, size_t length, int64_t rx_time,
                                int rssi, float snr, bool time_synced_at_rx,
                                bool link_was_down, uint32_t received_ms);

// MQTT task.
bool peekHead(gateway_frame_log::LiveFrame* out);
gateway_frame_log::Status freeze(uint32_t seq, bool buffered);
gateway_frame_log::Status acknowledge(uint32_t seq);
bool maintain();

size_t liveCount();
const Counters& counters();
// One or more "[<prefix>] ..." lines with state and counters (no payloads).
void printStatus(const char* prefix);

}  // namespace gateway_frame_store

#endif
