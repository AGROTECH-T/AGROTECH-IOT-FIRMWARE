#include "AgrotechBase64.h"

namespace agrotech {
namespace {

const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

size_t base64EncodedLength(size_t input_length) {
  return 4 * ((input_length + 2) / 3);
}

bool base64Encode(const uint8_t* data, size_t length, char* out,
                  size_t out_size) {
  if (out == nullptr) {
    return false;
  }
  if (length > 0 && data == nullptr) {
    return false;
  }

  const size_t encoded_length = base64EncodedLength(length);
  if (out_size < encoded_length + 1) {
    return false;
  }

  size_t in = 0;
  size_t o = 0;
  while (in + 3 <= length) {
    const uint32_t block = (static_cast<uint32_t>(data[in]) << 16) |
                           (static_cast<uint32_t>(data[in + 1]) << 8) |
                           static_cast<uint32_t>(data[in + 2]);
    out[o++] = kAlphabet[(block >> 18) & 0x3fu];
    out[o++] = kAlphabet[(block >> 12) & 0x3fu];
    out[o++] = kAlphabet[(block >> 6) & 0x3fu];
    out[o++] = kAlphabet[block & 0x3fu];
    in += 3;
  }

  const size_t remaining = length - in;
  if (remaining == 1) {
    const uint32_t block = static_cast<uint32_t>(data[in]) << 16;
    out[o++] = kAlphabet[(block >> 18) & 0x3fu];
    out[o++] = kAlphabet[(block >> 12) & 0x3fu];
    out[o++] = '=';
    out[o++] = '=';
  } else if (remaining == 2) {
    const uint32_t block = (static_cast<uint32_t>(data[in]) << 16) |
                           (static_cast<uint32_t>(data[in + 1]) << 8);
    out[o++] = kAlphabet[(block >> 18) & 0x3fu];
    out[o++] = kAlphabet[(block >> 12) & 0x3fu];
    out[o++] = kAlphabet[(block >> 6) & 0x3fu];
    out[o++] = '=';
  }

  out[o] = '\0';
  return true;
}

}  // namespace agrotech
