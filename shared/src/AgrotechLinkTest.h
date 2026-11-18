#ifndef AGROTECH_LINK_TEST_H
#define AGROTECH_LINK_TEST_H

#include <stdint.h>
#include <string.h>

#include "AgrotechProtocol.h"

// BUILD-IOT-03D link test support. This header never starts the radio by
// itself: the transport still requires AGROTECH_LORA_TRANSPORT_ENABLED and
// AGROTECH_LORA_PROFILE_AUTHORIZED plus a complete profile.
namespace agrotech_link_test {

// Controlled test mode. Off by default; enabling it does NOT authorize RF.
// The radio stays blocked until the shared profile is authorized and complete.
static const bool kEnabled = false;

// 0 = run until reset. >0 = stop scheduling after this many Frame v1 sends.
static const uint32_t kFrameLimit = 10;

// Synthetic readings used only in test mode so the link can be exercised
// without sensors. They are valid Frame v1 values, not measurements.
static const int32_t kTemperatureX100 = 2500;  // 25.00 C
static const int32_t kHumidityX100 = 5000;     // 50.00 %RH

struct TxStats {
  uint32_t encoded;
  uint32_t encode_failed;
  uint32_t started;
  uint32_t done;
  uint32_t busy;
  uint32_t write_failed;
  uint32_t start_failed;
  uint32_t timeouts;
};

struct RxStats {
  uint32_t frames;
  uint32_t malformed;
  uint32_t oversized;
  uint32_t incomplete;
  uint32_t lost;
  uint32_t duplicates;
  uint32_t out_of_order;
  uint8_t have_last;
  char uid[agrotech::kUidBytes + 1];
  uint32_t boot_counter;
  uint32_t seq;
  uint32_t first_rx_ms;
  uint32_t last_rx_ms;
};

// First four HMAC tag bytes, enough to match TX and RX logs without
// changing Frame v1 or adding bytes on air.
static inline uint32_t tagPrefix(const uint8_t* frame, size_t length) {
  if (frame == nullptr || length < agrotech::kTagBytes) return 0;
  const uint8_t* tag = frame + length - agrotech::kTagBytes;
  return (static_cast<uint32_t>(tag[0]) << 24) |
         (static_cast<uint32_t>(tag[1]) << 16) |
         (static_cast<uint32_t>(tag[2]) << 8) |
         static_cast<uint32_t>(tag[3]);
}

// LoRa airtime estimate in microseconds for an explicit-header frame.
// Returns 0 when the parameters are outside the SX1278 range.
static inline uint32_t airtimeUs(uint8_t spreading_factor, uint32_t bandwidth_hz,
                                 uint8_t coding_rate_denominator,
                                 uint16_t preamble_length, bool crc_enabled,
                                 size_t payload_bytes) {
  if (spreading_factor < 6 || spreading_factor > 12 || bandwidth_hz == 0 ||
      coding_rate_denominator < 5 || coding_rate_denominator > 8) {
    return 0;
  }
  const int32_t sf = spreading_factor;
  const uint32_t symbol_us =
      (static_cast<uint32_t>(1UL << sf) * 1000000UL) / bandwidth_hz;
  if (symbol_us == 0) return 0;

  uint64_t total_us =
      (static_cast<uint64_t>(preamble_length) * 4ULL + 17ULL) * symbol_us / 4ULL;

  const bool low_data_rate = (sf >= 11);
  const int32_t numerator = static_cast<int32_t>(8 * payload_bytes) - 4 * sf +
                            28 + (crc_enabled ? 16 : 0);
  const int32_t denominator = 4 * (sf - (low_data_rate ? 2 : 0));
  int32_t blocks = numerator / denominator;
  if (numerator % denominator != 0) ++blocks;
  if (blocks < 0) blocks = 0;
  const int32_t payload_symbols =
      8 + (static_cast<int32_t>(coding_rate_denominator) - 4) * blocks;

  total_us += static_cast<uint64_t>(payload_symbols) * symbol_us;
  return static_cast<uint32_t>((total_us + 999ULL) / 1000ULL);
}

// Sequence-gap accounting for the receiver. The first frame (or a frame from
// a different UID/boot) only sets the baseline; earlier history is unknown, so
// it is never counted as lost. Duplicates and reordering are reported
// separately and do not advance the baseline.
static inline void observeRxFrame(RxStats* stats, const char* uid,
                                  uint32_t boot_counter, uint32_t seq,
                                  uint32_t now_ms) {
  if (stats == nullptr || uid == nullptr) return;

  const bool same_stream =
      stats->have_last != 0 && strncmp(stats->uid, uid, agrotech::kUidBytes) == 0 &&
      stats->boot_counter == boot_counter;

  if (!same_stream) {
    stats->have_last = 1;
    strncpy(stats->uid, uid, agrotech::kUidBytes);
    stats->uid[agrotech::kUidBytes] = '\0';
    stats->boot_counter = boot_counter;
    stats->seq = seq;
    if (stats->first_rx_ms == 0) stats->first_rx_ms = now_ms;
  } else if (seq == stats->seq) {
    ++stats->duplicates;
  } else if (seq < stats->seq) {
    ++stats->out_of_order;
  } else {
    if (seq > stats->seq + 1) {
      stats->lost += seq - stats->seq - 1;
    }
    stats->seq = seq;
  }
  ++stats->frames;
  stats->last_rx_ms = now_ms;
}

}  // namespace agrotech_link_test

#endif
