#ifndef AGROTECH_PROTOCOL_H
#define AGROTECH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace agrotech {

constexpr uint8_t kFrameVersionV1 = 1;
constexpr size_t kHeaderBytes = 25;
constexpr size_t kReadingBytes = 5;
constexpr size_t kTagBytes = 32;
constexpr size_t kUidBytes = 13;
constexpr size_t kMinReadings = 1;
constexpr size_t kMaxReadingsV1 = 16;
constexpr size_t kMinFrameBytes =
    kHeaderBytes + kReadingBytes * kMinReadings + kTagBytes;
constexpr size_t kMaxFrameBytes =
    kHeaderBytes + kReadingBytes * kMaxReadingsV1 + kTagBytes;
constexpr size_t kHmacKeyBytes = 32;
constexpr size_t kHmacContextBytes = 22;

constexpr uint8_t kMetricCodeTemperature = 1;
constexpr uint8_t kMetricCodeRelativeHumidity = 2;
constexpr int32_t kTemperatureInvalidMarker = -32768;
constexpr int32_t kHumidityInvalidMarker = 65535;

extern const char kHmacContext[];

enum class Status : uint8_t {
  Ok = 0,
  InvalidArgument,
  InvalidUid,
  InvalidKeyVersion,
  InvalidReadingCount,
  InvalidChannel,
  DuplicateChannel,
  UnknownMetricCode,
  ValueOutOfRange,
  BufferTooSmall,
  UnsupportedVersion,
  Truncated,
  LengthMismatch,
  InvalidHmac,
  SequenceExhausted,
  BootCounterExhausted
};

const char* statusText(Status status);

struct Reading {
  uint16_t channel;
  uint8_t metric_code;
  int32_t value;
};

struct FrameFields {
  char coordinator_uid[kUidBytes + 1];
  uint16_t key_version;
  uint32_t boot_counter;
  uint32_t seq;
  uint8_t reading_count;
  Reading readings[kMaxReadingsV1];
};

struct ParsedFrame {
  FrameFields fields;
  const uint8_t* signed_bytes;
  size_t signed_length;
  const uint8_t* tag;
};

void initFrameFields(FrameFields* fields);

bool isValidUid(const char* uid, const char* prefix);
bool isKnownMetric(uint8_t metric_code);
bool isSupportedMetricValue(uint8_t metric_code, int32_t value);

size_t frameSize(size_t reading_count);

Status buildSignedBytes(const FrameFields& fields, uint8_t* out,
                        size_t out_size, size_t* out_length);
Status computeTag(const uint8_t key[kHmacKeyBytes],
                  const uint8_t* signed_bytes, size_t signed_length,
                  uint8_t out_tag[kTagBytes]);
Status encodeFrame(const FrameFields& fields, const uint8_t key[kHmacKeyBytes],
                   uint8_t* out, size_t out_size, size_t* out_length);

Status parseFrame(const uint8_t* data, size_t length, ParsedFrame* out);
Status verifyFrameTag(const uint8_t key[kHmacKeyBytes], const uint8_t* data,
                      size_t length);

}  // namespace agrotech

#endif
