#ifndef AGROTECH_BASE64_H
#define AGROTECH_BASE64_H

#include <stddef.h>
#include <stdint.h>

namespace agrotech {

size_t base64EncodedLength(size_t input_length);
bool base64Encode(const uint8_t* data, size_t length, char* out,
                  size_t out_size);

}  // namespace agrotech

#endif
