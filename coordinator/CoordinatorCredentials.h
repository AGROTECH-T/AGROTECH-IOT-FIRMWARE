#ifndef AGROTECH_COORDINATOR_CREDENTIALS_H
#define AGROTECH_COORDINATOR_CREDENTIALS_H

#include <stdint.h>

namespace coordinator_credentials {

bool loadOrProvision(char uid[14], uint16_t* key_version, uint8_t key[32]);

}  // namespace coordinator_credentials

#endif
