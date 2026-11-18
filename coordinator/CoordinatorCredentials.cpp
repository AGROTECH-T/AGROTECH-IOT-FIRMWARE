#include "CoordinatorCredentials.h"

#include <Arduino.h>
#include <Preferences.h>

#include <stdlib.h>
#include <string.h>

#include <AgrotechProtocol.h>

namespace coordinator_credentials {
namespace {

constexpr size_t kUidLength = 13;
constexpr size_t kKeyLength = 32;
constexpr size_t kBase64Length = 44;
constexpr size_t kLineCapacity = 96;

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool decodeKey(const char* text, uint8_t out[kKeyLength]) {
  if (text == nullptr || strlen(text) != kBase64Length || text[43] != '=') return false;
  size_t output = 0;
  for (size_t i = 0; i < 40; i += 4) {
    const int a = base64Value(text[i]);
    const int b = base64Value(text[i + 1]);
    const int c = base64Value(text[i + 2]);
    const int d = base64Value(text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    out[output++] = static_cast<uint8_t>((a << 2) | (b >> 4));
    out[output++] = static_cast<uint8_t>((b << 4) | (c >> 2));
    out[output++] = static_cast<uint8_t>((c << 6) | d);
  }
  const int a = base64Value(text[40]);
  const int b = base64Value(text[41]);
  const int c = base64Value(text[42]);
  if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0 || output != 30) return false;
  out[output++] = static_cast<uint8_t>((a << 2) | (b >> 4));
  out[output++] = static_cast<uint8_t>((b << 4) | (c >> 2));
  return output == kKeyLength;
}

void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (length--) *bytes++ = 0;
}

bool storeProvisioningLine(char* line, char uid[kUidLength + 1],
                           uint16_t* version, uint8_t key[kKeyLength]) {
  // Expected: SET <13-char CO UID> <uint16 version> <32-byte Base64 key>.
  if (strncmp(line, "SET ", 4) != 0) return false;
  char* identity = line + 4;
  char* first_space = strchr(identity, ' ');
  if (first_space == nullptr || first_space - identity != kUidLength) return false;
  *first_space = '\0';
  if (!agrotech::isValidUid(identity, "CO-")) return false;
  char* version_text = first_space + 1;
  char* second_space = strchr(version_text, ' ');
  if (second_space == nullptr || second_space == version_text) return false;
  *second_space = '\0';
  for (const char* p = version_text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  const unsigned long parsed = strtoul(version_text, nullptr, 10);
  if (parsed < 1 || parsed > 65535 || !decodeKey(second_space + 1, key)) return false;

  Preferences preferences;
  if (!preferences.begin("agro-co", false)) return false;
  const bool saved = preferences.putString("uid", identity) == kUidLength &&
                     preferences.putUShort("version", static_cast<uint16_t>(parsed)) == 2 &&
                     preferences.putBytes("hmac", key, kKeyLength) == kKeyLength;
  preferences.end();
  if (!saved) return false;
  memcpy(uid, identity, kUidLength + 1);
  *version = static_cast<uint16_t>(parsed);
  return true;
}

bool reprovisionRequested() {
  char command[16] = {};
  size_t length = 0;
  bool overflow = false;
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < 2000) {
    if (!Serial.available()) {
      delay(10);
      continue;
    }
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (length == 0) continue;
      command[length] = '\0';
      const bool requested = !overflow && strcmp(command, "REPROVISION") == 0;
      wipe(command, sizeof(command));
      return requested;
    }
    if (length + 1 < sizeof(command)) command[length++] = c;
    else overflow = true;
  }
  wipe(command, sizeof(command));
  return false;
}

}  // namespace

bool loadOrProvision(char uid[kUidLength + 1], uint16_t* key_version,
                     uint8_t key[kKeyLength]) {
  if (uid == nullptr || key_version == nullptr || key == nullptr) return false;
  Preferences preferences;
  if (!preferences.begin("agro-co", false)) return false;
  if (reprovisionRequested()) {
    preferences.clear();
    preferences.end();
    Serial.println("[PROVISION] Previous coordinator credentials cleared.");
    if (!preferences.begin("agro-co", false)) return false;
  }
  const bool present = preferences.getString("uid", uid, kUidLength + 1) == kUidLength + 1 &&  // F9c: length includes NUL
                       preferences.getUShort("version", 0) > 0 &&
                       preferences.getBytesLength("hmac") == kKeyLength &&
                       preferences.getBytes("hmac", key, kKeyLength) == kKeyLength;
  if (present) *key_version = preferences.getUShort("version", 0);
  preferences.end();
  if (present && agrotech::isValidUid(uid, "CO-") && *key_version > 0) return true;
  wipe(key, kKeyLength);

  Serial.println("[PROVISION] Enter SET <coordinator_uid> <key_version> <base64_key> over a trusted USB serial connection.");
  Serial.println("[PROVISION] Credential input is never echoed by firmware.");
  char line[kLineCapacity] = {};
  for (;;) {
    size_t length = 0;
    bool overflow = false;
    while (true) {
      while (!Serial.available()) delay(10);
      const char c = static_cast<char>(Serial.read());
      if (c == '\n' || c == '\r') {
        if (length > 0) break;
        continue;
      }
      if (length + 1 < sizeof(line)) line[length++] = c;
      else overflow = true;
    }
    line[length] = '\0';
    if (!overflow && storeProvisioningLine(line, uid, key_version, key)) {
      wipe(line, sizeof(line));
      Serial.println("[PROVISION] Credentials stored in ESP32 NVS.");
      return true;
    }
    wipe(line, sizeof(line));
    wipe(key, kKeyLength);
    Serial.println("[PROVISION] Rejected; check the format and try again.");
  }
}

}  // namespace coordinator_credentials
