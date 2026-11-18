#ifndef AGROTECH_MQTT_PAYLOAD_H
#define AGROTECH_MQTT_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "AgrotechProtocol.h"

namespace agrotech {

constexpr size_t kBase64FrameMaxBytes = 4 * ((kMaxFrameBytes + 2) / 3);
constexpr size_t kMqttPayloadMaxBytes = 512;

struct MqttPayloadInput {
  const uint8_t* frame;
  size_t frame_length;
  const char* lora_rx_at_iso;
  bool clock_synced;
  bool buffered;
  int rssi;
  float snr;
};

bool buildMqttPayload(const MqttPayloadInput& input, char* out,
                      size_t out_size);

}  // namespace agrotech

#endif
