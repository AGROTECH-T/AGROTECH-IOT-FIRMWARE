#include "AgrotechProtocol.h"

#include <string.h>

#include "AgrotechSha256.h"

namespace agrotech {

const char kHmacContext[] = "agrotech:lora-frame:v1";
static_assert(sizeof(kHmacContext) - 1 == kHmacContextBytes,
              "HMAC context must be 22 bytes");

namespace {

const char kUidAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                               static_cast<uint16_t>(data[1]));
}

uint32_t readU32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

void writeU16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}

void writeU32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value >> 24);
  data[1] = static_cast<uint8_t>(value >> 16);
  data[2] = static_cast<uint8_t>(value >> 8);
  data[3] = static_cast<uint8_t>(value);
}

int32_t decodeTemperature(uint16_t raw) {
  return (raw & 0x8000u) ? static_cast<int32_t>(raw) - 65536
                         : static_cast<int32_t>(raw);
}

}  // namespace

const char* statusText(Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::InvalidArgument:
      return "invalid_argument";
    case Status::InvalidUid:
      return "invalid_uid";
    case Status::InvalidKeyVersion:
      return "invalid_key_version";
    case Status::InvalidReadingCount:
      return "invalid_reading_count";
    case Status::InvalidChannel:
      return "invalid_channel";
    case Status::DuplicateChannel:
      return "duplicate_channel";
    case Status::UnknownMetricCode:
      return "unknown_metric_code";
    case Status::ValueOutOfRange:
      return "value_out_of_range";
    case Status::BufferTooSmall:
      return "buffer_too_small";
    case Status::UnsupportedVersion:
      return "unsupported_version";
    case Status::Truncated:
      return "truncated";
    case Status::LengthMismatch:
      return "length_mismatch";
    case Status::InvalidHmac:
      return "invalid_hmac";
    case Status::SequenceExhausted:
      return "sequence_exhausted";
    case Status::BootCounterExhausted:
      return "boot_counter_exhausted";
  }
  return "unknown_status";
}

void initFrameFields(FrameFields* fields) {
  if (fields != nullptr) {
    memset(fields, 0, sizeof(*fields));
  }
}

bool isValidUid(const char* uid, const char* prefix) {
  if (uid == nullptr || prefix == nullptr) {
    return false;
  }
  if (strlen(uid) != kUidBytes || strlen(prefix) != 3) {
    return false;
  }
  if (strncmp(uid, prefix, 3) != 0) {
    return false;
  }
  for (size_t i = 3; i < kUidBytes; ++i) {
    if (strchr(kUidAlphabet, uid[i]) == nullptr) {
      return false;
    }
  }
  return true;
}

bool isKnownMetric(uint8_t metric_code) {
  return metric_code == kMetricCodeTemperature ||
         metric_code == kMetricCodeRelativeHumidity;
}

bool isSupportedMetricValue(uint8_t metric_code, int32_t value) {
  if (metric_code == kMetricCodeTemperature) {
    return value >= -32768 && value <= 32767;
  }
  if (metric_code == kMetricCodeRelativeHumidity) {
    return (value >= 0 && value <= 10000) || value == 65535;
  }
  return false;
}

size_t frameSize(size_t reading_count) {
  return kHeaderBytes + kReadingBytes * reading_count + kTagBytes;
}

Status buildSignedBytes(const FrameFields& fields, uint8_t* out,
                        size_t out_size, size_t* out_length) {
  if (out == nullptr || out_length == nullptr) {
    return Status::InvalidArgument;
  }
  if (!isValidUid(fields.coordinator_uid, "CO-")) {
    return Status::InvalidUid;
  }
  if (fields.key_version < 1) {
    return Status::InvalidKeyVersion;
  }
  if (fields.reading_count < kMinReadings ||
      fields.reading_count > kMaxReadingsV1) {
    return Status::InvalidReadingCount;
  }

  const size_t signed_length =
      kHeaderBytes + kReadingBytes * static_cast<size_t>(fields.reading_count);
  if (out_size < signed_length) {
    return Status::BufferTooSmall;
  }

  for (size_t i = 0; i < fields.reading_count; ++i) {
    const Reading& reading = fields.readings[i];
    if (reading.channel < 1) {
      return Status::InvalidChannel;
    }
    if (!isKnownMetric(reading.metric_code)) {
      return Status::UnknownMetricCode;
    }
    for (size_t j = 0; j < i; ++j) {
      if (fields.readings[j].channel == reading.channel) {
        return Status::DuplicateChannel;
      }
    }
    if (!isSupportedMetricValue(reading.metric_code, reading.value)) {
      return Status::ValueOutOfRange;
    }
  }

  size_t offset = 0;
  out[offset++] = kFrameVersionV1;
  memcpy(out + offset, fields.coordinator_uid, kUidBytes);
  offset += kUidBytes;
  writeU16(out + offset, fields.key_version);
  offset += 2;
  writeU32(out + offset, fields.boot_counter);
  offset += 4;
  writeU32(out + offset, fields.seq);
  offset += 4;
  out[offset++] = fields.reading_count;

  for (size_t i = 0; i < fields.reading_count; ++i) {
    const Reading& reading = fields.readings[i];
    writeU16(out + offset, reading.channel);
    offset += 2;
    out[offset++] = reading.metric_code;
    writeU16(out + offset, static_cast<uint16_t>(reading.value));
    offset += 2;
  }

  *out_length = offset;
  return Status::Ok;
}

Status computeTag(const uint8_t key[kHmacKeyBytes],
                  const uint8_t* signed_bytes, size_t signed_length,
                  uint8_t out_tag[kTagBytes]) {
  if (key == nullptr || signed_bytes == nullptr || out_tag == nullptr) {
    return Status::InvalidArgument;
  }
  hmacSha256(key, kHmacKeyBytes,
             reinterpret_cast<const uint8_t*>(kHmacContext), kHmacContextBytes,
             signed_bytes, signed_length, out_tag);
  return Status::Ok;
}

Status encodeFrame(const FrameFields& fields, const uint8_t key[kHmacKeyBytes],
                   uint8_t* out, size_t out_size, size_t* out_length) {
  if (out == nullptr || out_length == nullptr) {
    return Status::InvalidArgument;
  }

  size_t signed_length = 0;
  Status status = buildSignedBytes(fields, out, out_size, &signed_length);
  if (status != Status::Ok) {
    return status;
  }
  if (out_size < signed_length + kTagBytes) {
    return Status::BufferTooSmall;
  }

  status = computeTag(key, out, signed_length, out + signed_length);
  if (status != Status::Ok) {
    return status;
  }

  *out_length = signed_length + kTagBytes;
  return Status::Ok;
}

Status parseFrame(const uint8_t* data, size_t length, ParsedFrame* out) {
  if (data == nullptr || out == nullptr) {
    return Status::InvalidArgument;
  }
  if (length == 0) {
    return Status::Truncated;
  }
  if (data[0] != kFrameVersionV1) {
    return Status::UnsupportedVersion;
  }
  if (length < kHeaderBytes) {
    return Status::Truncated;
  }

  const uint8_t reading_count = data[kHeaderBytes - 1];
  if (reading_count < kMinReadings || reading_count > kMaxReadingsV1) {
    return Status::InvalidReadingCount;
  }

  const size_t signed_length =
      kHeaderBytes + kReadingBytes * static_cast<size_t>(reading_count);
  if (length != signed_length + kTagBytes) {
    return Status::LengthMismatch;
  }

  char uid[kUidBytes + 1];
  memcpy(uid, data + 1, kUidBytes);
  uid[kUidBytes] = '\0';
  if (!isValidUid(uid, "CO-")) {
    return Status::InvalidUid;
  }

  const uint16_t key_version = readU16(data + 14);
  if (key_version < 1) {
    return Status::InvalidKeyVersion;
  }

  FrameFields fields;
  initFrameFields(&fields);
  memcpy(fields.coordinator_uid, uid, kUidBytes + 1);
  fields.key_version = key_version;
  fields.boot_counter = readU32(data + 16);
  fields.seq = readU32(data + 20);
  fields.reading_count = reading_count;

  for (size_t i = 0; i < reading_count; ++i) {
    const size_t offset = kHeaderBytes + kReadingBytes * i;
    const uint16_t channel = readU16(data + offset);
    const uint8_t metric_code = data[offset + 2];
    if (channel < 1) {
      return Status::InvalidChannel;
    }
    if (!isKnownMetric(metric_code)) {
      return Status::UnknownMetricCode;
    }
    for (size_t j = 0; j < i; ++j) {
      if (fields.readings[j].channel == channel) {
        return Status::DuplicateChannel;
      }
    }
    const uint16_t raw_value = readU16(data + offset + 3);
    const int32_t value = metric_code == kMetricCodeTemperature
                              ? decodeTemperature(raw_value)
                              : static_cast<int32_t>(raw_value);
    fields.readings[i].channel = channel;
    fields.readings[i].metric_code = metric_code;
    fields.readings[i].value = value;
  }

  out->fields = fields;
  out->signed_bytes = data;
  out->signed_length = signed_length;
  out->tag = data + signed_length;
  return Status::Ok;
}

Status verifyFrameTag(const uint8_t key[kHmacKeyBytes], const uint8_t* data,
                      size_t length) {
  if (key == nullptr) {
    return Status::InvalidArgument;
  }
  ParsedFrame parsed;
  Status status = parseFrame(data, length, &parsed);
  if (status != Status::Ok) {
    return status;
  }

  uint8_t expected[kTagBytes];
  status = computeTag(key, parsed.signed_bytes, parsed.signed_length, expected);
  if (status != Status::Ok) {
    return status;
  }

  uint8_t difference = 0;
  for (size_t i = 0; i < kTagBytes; ++i) {
    difference = static_cast<uint8_t>(difference | (expected[i] ^ parsed.tag[i]));
  }
  return difference == 0 ? Status::Ok : Status::InvalidHmac;
}

}  // namespace agrotech
