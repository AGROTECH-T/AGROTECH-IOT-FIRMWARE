#include "AgrotechSha256.h"

#include <string.h>

namespace agrotech {
namespace {

const uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t rotr(uint32_t value, uint32_t amount) {
  return (value >> amount) | (value << (32u - amount));
}

inline uint32_t load32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

inline void store32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value >> 24);
  data[1] = static_cast<uint8_t>(value >> 16);
  data[2] = static_cast<uint8_t>(value >> 8);
  data[3] = static_cast<uint8_t>(value);
}

}  // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  bit_length_ = 0;
  buffer_length_ = 0;
}

void Sha256::transform(const uint8_t block[64]) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = load32(block + 4 * i);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  uint32_t f = state_[5];
  uint32_t g = state_[6];
  uint32_t h = state_[7];

  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t length) {
  if (length == 0) {
    return;
  }
  bit_length_ += static_cast<uint64_t>(length) * 8u;
  size_t offset = 0;
  while (offset < length) {
    const size_t space = 64 - buffer_length_;
    const size_t remaining = length - offset;
    const size_t take = remaining < space ? remaining : space;
    memcpy(buffer_ + buffer_length_, data + offset, take);
    buffer_length_ += take;
    offset += take;
    if (buffer_length_ == 64) {
      transform(buffer_);
      buffer_length_ = 0;
    }
  }
}

void Sha256::finish(uint8_t out[32]) {
  const uint64_t bits = bit_length_;
  buffer_[buffer_length_++] = 0x80;

  if (buffer_length_ > 56) {
    while (buffer_length_ < 64) {
      buffer_[buffer_length_++] = 0;
    }
    transform(buffer_);
    buffer_length_ = 0;
  }

  while (buffer_length_ < 56) {
    buffer_[buffer_length_++] = 0;
  }

  for (size_t i = 0; i < 8; ++i) {
    buffer_[56 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  }

  transform(buffer_);
  for (size_t i = 0; i < 8; ++i) {
    store32(out + 4 * i, state_[i]);
  }
  buffer_length_ = 0;
}

void hmacSha256(const uint8_t* key, size_t key_length,
                const uint8_t* part1, size_t part1_length,
                const uint8_t* part2, size_t part2_length,
                uint8_t out[32]) {
  uint8_t key_block[64];
  memset(key_block, 0, sizeof(key_block));

  if (key_length > sizeof(key_block)) {
    Sha256 key_hash;
    key_hash.update(key, key_length);
    key_hash.finish(key_block);
  } else if (key_length > 0) {
    memcpy(key_block, key, key_length);
  }

  uint8_t inner_pad[64];
  uint8_t outer_pad[64];
  for (size_t i = 0; i < sizeof(key_block); ++i) {
    inner_pad[i] = static_cast<uint8_t>(key_block[i] ^ 0x36u);
    outer_pad[i] = static_cast<uint8_t>(key_block[i] ^ 0x5cu);
  }

  uint8_t inner_hash[32];
  Sha256 inner;
  inner.update(inner_pad, sizeof(inner_pad));
  if (part1_length > 0) {
    inner.update(part1, part1_length);
  }
  if (part2_length > 0) {
    inner.update(part2, part2_length);
  }
  inner.finish(inner_hash);

  Sha256 outer;
  outer.update(outer_pad, sizeof(outer_pad));
  outer.update(inner_hash, sizeof(inner_hash));
  outer.finish(out);
}

}  // namespace agrotech
