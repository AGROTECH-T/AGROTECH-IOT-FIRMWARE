#include "GatewayCredentials.h"

#include <Arduino.h>
#include <Preferences.h>

#include <string.h>

#include <AgrotechProtocol.h>

namespace gateway_credentials {
namespace {

constexpr const char* kNamespace = "agro-gw";
constexpr size_t kLineCapacity = 256;
// SET <TAB> gateway_uid <TAB> mqtt_username <TAB> mqtt_password <TAB>
// wifi_ssid <TAB> wifi_password. Field count, order and meaning are fixed.
constexpr size_t kSetFieldCount = 6;
constexpr size_t kLongestFieldLength = 64;

// Set-integrity marker (not a secret). The five credential keys are accepted
// only while this key holds kSetComplete. SET first overwrites it with
// kSetPending (verified), then writes the five fields, and writes kSetComplete
// last. A reset or failure at any point therefore leaves either the previous
// complete set untouched, or a set marked pending, which boot rejects.
constexpr const char* kSetMarkerKey = "cred_state";
constexpr const char* kSetPending = "pending";
constexpr const char* kSetComplete = "complete-v1";
constexpr size_t kSetMarkerMaxLength = 16;

static_assert(kGatewayUidLength == agrotech::kUidBytes,
              "gateway UID length must match the shared UID contract");
static_assert(kMqttUsernameLength <= kLongestFieldLength &&
                  kMqttPasswordMaxLength <= kLongestFieldLength &&
                  kWifiSsidMaxLength <= kLongestFieldLength &&
                  kWifiPasswordMaxLength <= kLongestFieldLength &&
                  kSetMarkerMaxLength <= kLongestFieldLength,
              "read-back buffer must hold every credential field");
static_assert(3 + (kSetFieldCount - 1) + kGatewayUidLength +
                      kMqttUsernameLength + kMqttPasswordMaxLength +
                      kWifiSsidMaxLength + kWifiPasswordMaxLength <
                  kLineCapacity,
              "a maximal SET line must fit the serial line buffer");

void wipe(void* data, size_t length) {
  volatile char* bytes = static_cast<volatile char*>(data);
  while (length--) *bytes++ = 0;
}

bool tokenCharacters(const char* value) {
  for (const char* p = value; *p; ++p) {
    const char c = *p;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

bool validMqttUsername(const char* value) {
  return value != nullptr && strlen(value) == kMqttUsernameLength &&
         tokenCharacters(value);
}

// Flespi uses an empty MQTT password; anything else is rejected (fail-closed).
bool validMqttPassword(const char* value) {
  return value != nullptr && strlen(value) <= kMqttPasswordMaxLength;
}

bool validWifiSsid(const char* value) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  return length > 0 && length <= kWifiSsidMaxLength;
}

// An empty Wi-Fi password (open network) keeps the previous 0..64 limit.
bool validWifiPassword(const char* value) {
  return value != nullptr && strlen(value) <= kWifiPasswordMaxLength;
}

bool validCredentials(const char* uid, const char* username,
                      const char* password, const char* wifi_ssid,
                      const char* wifi_password) {
  // The UID is validated on its own; it is not related to the username.
  return agrotech::isValidUid(uid, "GW-") && validMqttUsername(username) &&
         validMqttPassword(password) && validWifiSsid(wifi_ssid) &&
         validWifiPassword(wifi_password);
}

// Splits on every TAB, keeping empty fields (unlike strtok_r, which merges
// consecutive delimiters and shifts later fields). Requires exactly `count`
// fields; fields point into `line`, whose TABs are replaced by NUL.
bool splitFields(char* line, char* fields[], size_t count) {
  size_t found = 0;
  char* start = line;
  for (char* p = line;; ++p) {
    if (*p != '\t' && *p != '\0') continue;
    if (found == count) return false;  // more fields than the format allows
    const bool last = *p == '\0';
    *p = '\0';
    fields[found++] = start;
    if (last) break;
    start = p + 1;
  }
  return found == count;
}

// Preferences::getString(key, buf, size) returns the stored length INCLUDING
// the NUL terminator (nvs_get_str semantics): 1 for "", 14 for a 13-char UID.
// It returns 0 when the key is missing, unreadable or larger than `out_size`;
// it never truncates. Accepts only content lengths in [0, max_length].
bool readStored(Preferences& preferences, const char* key, char* out,
                size_t out_size, size_t max_length) {
  if (out_size < max_length + 1) return false;
  const size_t stored = preferences.getString(key, out, out_size);
  if (stored == 0 || stored > max_length + 1) return false;
  return out[stored - 1] == '\0' && strlen(out) == stored - 1;
}

// putString() returns strlen(value) on success and 0 on failure, which is
// ambiguous for an empty value, so every write is confirmed by reading it back.
bool writeVerified(Preferences& preferences, const char* key,
                   const char* value, size_t max_length) {
  const size_t length = strlen(value);
  if (length > max_length) return false;
  if (preferences.putString(key, value) != length) return false;
  char check[kLongestFieldLength + 1] = {};
  const bool same = readStored(preferences, key, check, sizeof(check), max_length) &&
                    strcmp(check, value) == 0;
  wipe(check, sizeof(check));
  return same;
}

bool copyField(char* out, size_t out_size, const char* value) {
  const size_t length = strlen(value);
  if (length >= out_size) return false;
  memcpy(out, value, length + 1);
  return true;
}

bool store(char* line, char* uid, size_t uid_size, char* username,
           size_t username_size, char* password, size_t password_size,
           char* wifi_ssid, size_t wifi_ssid_size, char* wifi_password,
           size_t wifi_password_size) {
  // TAB-separated fields allow spaces in the Wi-Fi SSID and password.
  char* fields[kSetFieldCount] = {};
  if (!splitFields(line, fields, kSetFieldCount)) return false;
  if (strcmp(fields[0], "SET") != 0) return false;
  const char* id = fields[1];
  const char* user = fields[2];
  const char* secret = fields[3];
  const char* ssid = fields[4];
  const char* wifi_secret = fields[5];
  // Everything is validated before NVS is touched.
  if (!validCredentials(id, user, secret, ssid, wifi_secret)) return false;
  if (strlen(id) >= uid_size || strlen(user) >= username_size ||
      strlen(secret) >= password_size || strlen(ssid) >= wifi_ssid_size ||
      strlen(wifi_secret) >= wifi_password_size) return false;

  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  // 1) Invalidate the stored set before touching any credential key.
  if (!writeVerified(preferences, kSetMarkerKey, kSetPending, kSetMarkerMaxLength)) {
    preferences.end();
    Serial.println("[PROVISION] Integrity marker could not be invalidated; no credential field was modified.");
    return false;
  }
  // 2) Write and read back the five fields; 3) only then mark the set complete.
  const bool saved =
      writeVerified(preferences, "uid", id, kGatewayUidLength) &&
      writeVerified(preferences, "mqtt_user", user, kMqttUsernameLength) &&
      writeVerified(preferences, "mqtt_pass", secret, kMqttPasswordMaxLength) &&
      writeVerified(preferences, "wifi_ssid", ssid, kWifiSsidMaxLength) &&
      writeVerified(preferences, "wifi_pass", wifi_secret, kWifiPasswordMaxLength) &&
      writeVerified(preferences, kSetMarkerKey, kSetComplete, kSetMarkerMaxLength);
  preferences.end();
  if (!saved) {
    // The marker is left pending (or was never confirmed complete): boot
    // rejects this partial set and asks for a new SET. No recovery attempted.
    Serial.println("[PROVISION] NVS write failed; stored credentials are marked incomplete.");
    return false;
  }
  return copyField(uid, uid_size, id) &&
         copyField(username, username_size, user) &&
         copyField(password, password_size, secret) &&
         copyField(wifi_ssid, wifi_ssid_size, ssid) &&
         copyField(wifi_password, wifi_password_size, wifi_secret);
}

bool reprovisionRequested() {
  char command[16] = {};
  size_t length = 0;
  bool overflow = false;
  bool nul_byte = false;
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < 2000) {
    if (!Serial.available()) {
      delay(10);
      continue;
    }
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      // A line made only of NUL bytes also ends here (and is rejected).
      if (length == 0 && !nul_byte) continue;
      command[length] = '\0';
      // A NUL would make strcmp see only the text before it (for example
      // "REPROVISION\0xyz"), so any line containing one never clears NVS.
      const bool requested =
          !overflow && !nul_byte && strcmp(command, "REPROVISION") == 0;
      wipe(command, sizeof(command));
      return requested;
    }
    if (c == '\0') {
      nul_byte = true;
      continue;
    }
    if (length + 1 < sizeof(command)) command[length++] = c;
    else overflow = true;
  }
  wipe(command, sizeof(command));
  return false;
}

}  // namespace

bool loadOrProvision(char* uid, size_t uid_size, char* username,
                     size_t username_size, char* password,
                     size_t password_size, char* wifi_ssid,
                     size_t wifi_ssid_size, char* wifi_password,
                     size_t wifi_password_size) {
  if (uid == nullptr || username == nullptr || password == nullptr ||
      wifi_ssid == nullptr || wifi_password == nullptr ||
      uid_size < kGatewayUidLength + 1 ||
      username_size < kMqttUsernameLength + 1 ||
      password_size < kMqttPasswordMaxLength + 1 ||
      wifi_ssid_size < kWifiSsidMaxLength + 1 ||
      wifi_password_size < kWifiPasswordMaxLength + 1) return false;
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  if (reprovisionRequested()) {
    preferences.clear();
    preferences.end();
    Serial.println("[PROVISION] Previous gateway credentials cleared.");
    if (!preferences.begin(kNamespace, false)) return false;
  }
  // The set is accepted only if its integrity marker reads exactly
  // kSetComplete; a missing, empty, pending or unknown marker fails closed.
  char marker[kSetMarkerMaxLength + 1] = {};
  const bool complete =
      readStored(preferences, kSetMarkerKey, marker, sizeof(marker), kSetMarkerMaxLength) &&
      strcmp(marker, kSetComplete) == 0;
  const bool present =
      complete &&
      readStored(preferences, "uid", uid, uid_size, kGatewayUidLength) &&
      readStored(preferences, "mqtt_user", username, username_size, kMqttUsernameLength) &&
      readStored(preferences, "mqtt_pass", password, password_size, kMqttPasswordMaxLength) &&
      readStored(preferences, "wifi_ssid", wifi_ssid, wifi_ssid_size, kWifiSsidMaxLength) &&
      readStored(preferences, "wifi_pass", wifi_password, wifi_password_size, kWifiPasswordMaxLength);
  preferences.end();
  if (present &&
      validCredentials(uid, username, password, wifi_ssid, wifi_password)) return true;
  // Fail closed: RAM copies are wiped, but NVS is left untouched until a
  // valid SET overwrites it (no automatic erase, no migration).
  wipe(uid, uid_size);
  wipe(username, username_size);
  wipe(password, password_size);
  wipe(wifi_ssid, wifi_ssid_size);
  wipe(wifi_password, wifi_password_size);

  Serial.println("[PROVISION] No valid gateway credentials in NVS (missing, incomplete, unreadable or previous format).");
  Serial.println("[PROVISION] Send tab-separated SET, gateway_uid, mqtt_username (64-char Flespi token), mqtt_password (empty field), Wi-Fi SSID and Wi-Fi password over trusted USB serial.");
  Serial.println("[PROVISION] Credential input is never echoed by firmware.");
  char line[kLineCapacity] = {};
  for (;;) {
    size_t length = 0;
    bool overflow = false;
    bool nul_byte = false;
    while (true) {
      while (!Serial.available()) delay(10);
      const char c = static_cast<char>(Serial.read());
      if (c == '\n' || c == '\r') {
        // A line made only of NUL bytes must also end here, so the flag
        // never leaks into the next line.
        if (length > 0 || nul_byte) break;
        continue;
      }
      if (c == '\0') {
        // A NUL would end the C string early and silently truncate a field:
        // the whole line is rejected at its end of line, never parsed.
        nul_byte = true;
        continue;
      }
      if (length + 1 < sizeof(line)) line[length++] = c;
      else overflow = true;
    }
    line[length] = '\0';
    if (nul_byte) {
      wipe(line, sizeof(line));
      wipe(uid, uid_size);
      wipe(username, username_size);
      wipe(password, password_size);
      wipe(wifi_ssid, wifi_ssid_size);
      wipe(wifi_password, wifi_password_size);
      Serial.println("[PROVISION] Rejected; input contained a NUL byte (line discarded, not echoed).");
      continue;
    }
    if (!overflow &&
        store(line, uid, uid_size, username, username_size, password,
              password_size, wifi_ssid, wifi_ssid_size, wifi_password,
              wifi_password_size)) {
      wipe(line, sizeof(line));
      Serial.println("[PROVISION] Gateway credentials stored in ESP32 NVS.");
      return true;
    }
    wipe(line, sizeof(line));
    wipe(uid, uid_size);
    wipe(username, username_size);
    wipe(password, password_size);
    wipe(wifi_ssid, wifi_ssid_size);
    wipe(wifi_password, wifi_password_size);
    Serial.println("[PROVISION] Rejected; check the format and try again.");
  }
}

}  // namespace gateway_credentials
