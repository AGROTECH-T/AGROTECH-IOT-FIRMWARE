#ifndef AGROTECH_SHA256_H
#define AGROTECH_SHA256_H

#include <stddef.h>
#include <stdint.h>

namespace agrotech {

class Sha256 {
 public:
  Sha256();
  void reset();
  void update(const uint8_t* data, size_t length);
  void finish(uint8_t out[32]);

 private:
  uint32_t state_[8];
  uint64_t bit_length_;
  uint8_t buffer_[64];
  size_t buffer_length_;
  void transform(const uint8_t block[64]);
};

void hmacSha256(const uint8_t* key, size_t key_length,
                const uint8_t* part1, size_t part1_length,
                const uint8_t* part2, size_t part2_length,
                uint8_t out[32]);

}  // namespace agrotech

#endif
