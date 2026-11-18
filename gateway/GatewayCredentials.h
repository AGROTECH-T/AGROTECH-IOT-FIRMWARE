#ifndef AGROTECH_GATEWAY_CREDENTIALS_H
#define AGROTECH_GATEWAY_CREDENTIALS_H

#include <stddef.h>

namespace gateway_credentials {

// Content lengths, excluding the NUL terminator. Caller buffers must hold
// at least <limit> + 1 bytes; loadOrProvision() rejects smaller buffers.
//
// Flespi authentication scheme: the MQTT username is the gateway's individual
// 64-character Flespi ACL token and the MQTT password is empty. The gateway
// UID (GW-XXXXXXXXXX) is an independent identity used for the topic and the
// client id; it is never compared with the username.
constexpr size_t kGatewayUidLength = 13;
constexpr size_t kMqttUsernameLength = 64;
constexpr size_t kMqttPasswordMaxLength = 0;
constexpr size_t kWifiSsidMaxLength = 32;
constexpr size_t kWifiPasswordMaxLength = 64;

bool loadOrProvision(char* uid, size_t uid_size, char* username,
                     size_t username_size, char* password,
                     size_t password_size, char* wifi_ssid,
                     size_t wifi_ssid_size, char* wifi_password,
                     size_t wifi_password_size);

}  // namespace gateway_credentials

#endif
