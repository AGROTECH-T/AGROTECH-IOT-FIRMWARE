#include "AgrotechMqttPayload.h"

#include <stdio.h>
#include <string.h>

#include "AgrotechBase64.h"

namespace agrotech {

bool buildMqttPayload(const MqttPayloadInput& input, char* out,
                      size_t out_size) {
  if (out == nullptr || input.frame == nullptr ||
      input.frame_length > kMaxFrameBytes) {
    return false;
  }

  char frame_base64[kBase64FrameMaxBytes + 1];
  if (!base64Encode(input.frame, input.frame_length, frame_base64,
                    sizeof(frame_base64))) {
    return false;
  }

  const bool clock_synced =
      input.clock_synced && input.lora_rx_at_iso != nullptr;
  char lora_rx_at[64];
  if (clock_synced) {
    const int written = snprintf(lora_rx_at, sizeof(lora_rx_at), "\"%s\"",
                                 input.lora_rx_at_iso);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(lora_rx_at)) {
      return false;
    }
  } else {
    strcpy(lora_rx_at, "null");
  }

  const int written = snprintf(
      out, out_size,
      "{\"v\":1,\"frame\":\"%s\",\"lora_rx_at\":%s,\"clock_synced\":%s,"
      "\"buffered\":%s,\"rssi\":%d,\"snr\":%.2f}",
      frame_base64, lora_rx_at, clock_synced ? "true" : "false",
      input.buffered ? "true" : "false", input.rssi,
      static_cast<double>(input.snr));

  return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace agrotech
