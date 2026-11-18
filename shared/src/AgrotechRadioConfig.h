#ifndef AGROTECH_RADIO_CONFIG_H
#define AGROTECH_RADIO_CONFIG_H

#include <stdint.h>

// ===========================================================================
// AGROTECH shared LoRa profile (single source of truth for coordinator and
// gateway). This file prepares the radio profile only. It does not start the
// radio: activation requires BOTH flags below plus a complete, coherent,
// electrically confirmed profile.
//
// Status: PROPOSED, NOT AUTHORIZED.
// See docs/BUILD-IOT-03C-LORA-PROFILE.md for the regulatory/RF rationale and
// the list of values still pending confirmation.
// ===========================================================================

// Two independent gates. The transport flag expresses build intent; the
// authorization flag records regulatory/RF sign-off. Never enable either one
// with placeholder values.
// MVP-IOT-F9 (2026-11-18): the owner authorized the candidate profile below
// (433.5 MHz, 7 dBm, SF7, BW125, CR4/5, sync 0xA6) for BENCH / MVP
// demonstration only. Field deployment requires its own regulatory review.
static const bool AGROTECH_LORA_TRANSPORT_ENABLED = true;
static const bool AGROTECH_LORA_PROFILE_AUTHORIZED = true;

// Unrelated to this build phase; MQTT/Wi-Fi stay disabled as well.
// MVP-IOT-F9: production MQTT path of the gateway (durable SPIFFS store).
static const bool AGROTECH_MQTT_TRANSPORT_ENABLED = true;

// Profile lifecycle. Historic = legacy pair, Proposed = candidate documented in
// BUILD-IOT-03C, Authorized = signed off for transmission.
enum class AgrotechRadioProfileStatus : uint8_t {
  Historic = 0,
  Proposed = 1,
  Authorized = 2,
};

static const AgrotechRadioProfileStatus AGROTECH_LORA_PROFILE_STATUS =
    AgrotechRadioProfileStatus::Authorized;  // MVP-IOT-F9

// Bump when any operational radio constant changes so both devices can log a
// different profile signature.
// Revision 2 (DEMO-02B): operational constants filled with the candidate
// profile below; still NOT authorized (flags false, status Proposed).
static const uint8_t AGROTECH_LORA_PROFILE_REVISION = 2;

// Wiring confirmed by BUILD-IOT-03B (SPI VERSION = 0x12, 10/10 on both ESP32s).
static const int AGROTECH_LORA_PIN_NSS = 5;
static const int AGROTECH_LORA_PIN_RESET = 14;
static const int AGROTECH_LORA_PIN_DIO0 = 26;
static const int AGROTECH_LORA_PIN_SCK = 18;
static const int AGROTECH_LORA_PIN_MOSI = 23;
static const int AGROTECH_LORA_PIN_MISO = 19;

// ---------------------------------------------------------------------------
// Historic values. Reference only: NOT approved and NEVER consumed by the
// transport. Kept to document where the legacy pair came from.
// ---------------------------------------------------------------------------
namespace agrotech_radio_historic {
static const uint32_t kFrequencyHz = 433000000UL;
static const uint8_t kSpreadingFactor = 7;
static const uint32_t kBandwidthHz = 125000UL;
static const uint8_t kCodingRateDenominator = 5;
static const int kTxPowerDbm = 17;  // legacy conducted value, not reused
static const int kSyncWord = 0xA6;
}  // namespace agrotech_radio_historic

// ---------------------------------------------------------------------------
// Proposed candidate profile. NOT authorized for transmission. Since
// DEMO-02B the operational constants below are taken from these values, so
// both devices share them; the activation gates (flags + status) still keep
// the radio off. Values are imported explicitly, never with placeholders.
//   - Frequency: candidate inside the 433-434.79 MHz ANE free-use table, kept
//     away from both band edges for 125 kHz BW; still requires confirmation.
//   - TX power: 7 dBm conducted is a proposal derived from the 10 mW P.R.A.
//     limit assuming a 0 dBd antenna and 0.5 dB feed-line loss
//     (7 + 0 - 0.5 = 6.5 dBm P.R.A. ~= 4.5 mW).
//   - Preamble 8 and CRC on are LoRa defaults, not regulatory values.
// ---------------------------------------------------------------------------
namespace agrotech_radio_proposed {
static const uint32_t kFrequencyHz = 433500000UL;
static const uint8_t kSpreadingFactor = 7;
static const uint32_t kBandwidthHz = 125000UL;
static const uint8_t kCodingRateDenominator = 5;
static const int kTxPowerDbm = 7;
static const int kSyncWord = 0xA6;
static const uint16_t kPreambleLength = 8;
static const int kUseCrc = 1;
static const uint32_t kMaxPraMilliwatts = 10;
}  // namespace agrotech_radio_proposed

// ---------------------------------------------------------------------------
// Operational profile. These are the only values the transport reads.
// DEMO-02B: filled with the candidate profile (agrotech_radio_proposed) so
// both devices are prepared with identical parameters and signature. A
// complete profile does NOT authorize transmission: activation still requires
// AGROTECH_LORA_TRANSPORT_ENABLED, AGROTECH_LORA_PROFILE_AUTHORIZED and
// AGROTECH_LORA_PROFILE_STATUS == Authorized (see
// agrotechRadioActivationBlockReason and the static_asserts below). The
// conducted power is a candidate pending physical verification.
// ---------------------------------------------------------------------------
static const uint32_t AGROTECH_LORA_FREQUENCY_HZ = agrotech_radio_proposed::kFrequencyHz;
static const uint8_t AGROTECH_LORA_SPREADING_FACTOR =
    agrotech_radio_proposed::kSpreadingFactor;
static const uint32_t AGROTECH_LORA_BANDWIDTH_HZ = agrotech_radio_proposed::kBandwidthHz;
static const uint8_t AGROTECH_LORA_CODING_RATE_DENOMINATOR =
    agrotech_radio_proposed::kCodingRateDenominator;
static const int AGROTECH_LORA_TX_POWER_DBM = agrotech_radio_proposed::kTxPowerDbm;
static const int AGROTECH_LORA_SYNC_WORD = agrotech_radio_proposed::kSyncWord;
static const uint16_t AGROTECH_LORA_PREAMBLE_LENGTH = agrotech_radio_proposed::kPreambleLength;
static const int AGROTECH_LORA_USE_CRC = agrotech_radio_proposed::kUseCrc;

// SX1276/77/78/79 tunable range (Hz) as implemented by the LoRa driver.
static const uint32_t AGROTECH_LORA_MIN_FREQUENCY_HZ = 137000000UL;
static const uint32_t AGROTECH_LORA_MAX_FREQUENCY_HZ = 1020000000UL;

// Enabling the transport without an authorized profile is a build error.
static_assert(!AGROTECH_LORA_TRANSPORT_ENABLED ||
                  AGROTECH_LORA_PROFILE_AUTHORIZED,
              "LoRa transport cannot be enabled without an authorized profile");
// DEMO-02B: the operational profile is now complete, so profile completeness
// is no longer a barrier. The lifecycle status becomes one instead: the
// authorization flag can only be set together with status Authorized.
static_assert(!AGROTECH_LORA_PROFILE_AUTHORIZED ||
                  AGROTECH_LORA_PROFILE_STATUS == AgrotechRadioProfileStatus::Authorized,
              "LoRa profile cannot be authorized while its status is not Authorized");

// Gateway LED GPIO was used by the legacy sketch. HIGH/LOW follows that code;
// current physical polarity/wiring has not been independently verified.
static const int AGROTECH_GATEWAY_LED_PIN = 25;
static const uint8_t AGROTECH_GATEWAY_LED_ON_LEVEL = 1;
static const uint8_t AGROTECH_GATEWAY_LED_OFF_LEVEL = 0;

// ---------------------------------------------------------------------------
// Shared validation helpers. Both sketches call these, so the acceptance
// criteria cannot drift apart.
// ---------------------------------------------------------------------------
static inline bool agrotechRadioBandwidthSupported(uint32_t bandwidth_hz) {
  switch (bandwidth_hz) {
    case 7800:
    case 10400:
    case 15600:
    case 20800:
    case 31250:
    case 41700:
    case 62500:
    case 125000:
    case 250000:
    case 500000:
      return true;
    default:
      return false;
  }
}

static inline bool agrotechRadioGpioUsable(int pin) {
  if (pin < 0 || pin > 39) return false;
  if (pin >= 6 && pin <= 11) return false;  // SPI flash pins
  return true;
}

static inline bool agrotechRadioGpioOutputCapable(int pin) {
  if (!agrotechRadioGpioUsable(pin)) return false;
  return pin < 34;  // GPIO34-39 are input-only
}

static inline bool agrotechRadioPinsValid() {
  const int pins[6] = {AGROTECH_LORA_PIN_NSS,   AGROTECH_LORA_PIN_RESET,
                       AGROTECH_LORA_PIN_DIO0,  AGROTECH_LORA_PIN_SCK,
                       AGROTECH_LORA_PIN_MOSI,  AGROTECH_LORA_PIN_MISO};
  for (int i = 0; i < 6; ++i) {
    if (!agrotechRadioGpioUsable(pins[i])) return false;
    for (int j = 0; j < i; ++j) {
      if (pins[i] == pins[j]) return false;
    }
  }
  return agrotechRadioGpioOutputCapable(AGROTECH_LORA_PIN_NSS) &&
         agrotechRadioGpioOutputCapable(AGROTECH_LORA_PIN_RESET) &&
         agrotechRadioGpioOutputCapable(AGROTECH_LORA_PIN_SCK) &&
         agrotechRadioGpioOutputCapable(AGROTECH_LORA_PIN_MOSI);
}

static inline bool agrotechRadioProfileValid() {
  return AGROTECH_LORA_FREQUENCY_HZ >= AGROTECH_LORA_MIN_FREQUENCY_HZ &&
         AGROTECH_LORA_FREQUENCY_HZ <= AGROTECH_LORA_MAX_FREQUENCY_HZ &&
         AGROTECH_LORA_SPREADING_FACTOR >= 6 &&
         AGROTECH_LORA_SPREADING_FACTOR <= 12 &&
         agrotechRadioBandwidthSupported(AGROTECH_LORA_BANDWIDTH_HZ) &&
         AGROTECH_LORA_CODING_RATE_DENOMINATOR >= 5 &&
         AGROTECH_LORA_CODING_RATE_DENOMINATOR <= 8 &&
         AGROTECH_LORA_TX_POWER_DBM >= 0 &&
         AGROTECH_LORA_TX_POWER_DBM <= 20 &&
         AGROTECH_LORA_SYNC_WORD >= 0 &&
         AGROTECH_LORA_SYNC_WORD <= 255 &&
         AGROTECH_LORA_PREAMBLE_LENGTH >= 1 &&
         (AGROTECH_LORA_USE_CRC == 0 || AGROTECH_LORA_USE_CRC == 1);
}

// Returns nullptr when the radio may start, otherwise the blocking reason.
static inline const char *agrotechRadioActivationBlockReason() {
  if (!AGROTECH_LORA_TRANSPORT_ENABLED) return "transport flag is false";
  if (!AGROTECH_LORA_PROFILE_AUTHORIZED)
    return "profile is not authorized for transmission";
  if (AGROTECH_LORA_PROFILE_STATUS != AgrotechRadioProfileStatus::Authorized)
    return "profile status is not Authorized";
  if (!agrotechRadioPinsValid()) return "LoRa pin map is invalid";
  if (!agrotechRadioProfileValid())
    return "LoRa RF profile is incomplete or out of range";
  return nullptr;
}

static inline bool agrotechRadioCanActivate() {
  return agrotechRadioActivationBlockReason() == nullptr;
}

static inline uint32_t agrotechRadioHashU32(uint32_t hash, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) {
    hash = (hash ^ ((value >> (8 * i)) & 0xFFUL)) * 16777619UL;
  }
  return hash;
}

// FNV-1a over the operational profile. Coordinator and gateway must log the
// same value; the shared header guarantees it, the log makes it verifiable.
static inline uint32_t agrotechRadioProfileSignature() {
  uint32_t hash = 2166136261UL;
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_PROFILE_REVISION);
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_FREQUENCY_HZ);
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_SPREADING_FACTOR);
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_BANDWIDTH_HZ);
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_CODING_RATE_DENOMINATOR);
  hash = agrotechRadioHashU32(hash,
                              static_cast<uint32_t>(AGROTECH_LORA_TX_POWER_DBM));
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_SYNC_WORD);
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_PREAMBLE_LENGTH);
  hash = agrotechRadioHashU32(hash, AGROTECH_LORA_USE_CRC);
  return hash;
}

#endif
