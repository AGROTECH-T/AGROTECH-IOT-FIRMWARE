#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "AgrotechBase64.h"
#include "AgrotechCounters.h"
#include "AgrotechMqttPayload.h"
#include "AgrotechProtocol.h"
#include "AgrotechSha256.h"
#include "CoordinatorConfig.h"
#include "CoordinatorSensorMath.h"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                   \
  do {                                                                     \
    ++checks;                                                              \
    if (!(condition)) {                                                    \
      ++failures;                                                          \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
    }                                                                      \
  } while (0)

int hexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool hexToBytes(const char* hex, uint8_t* out, size_t out_size,
                size_t* out_length) {
  const size_t length = strlen(hex);
  if (length % 2 != 0 || length / 2 > out_size) {
    return false;
  }
  for (size_t i = 0; i < length / 2; ++i) {
    const int high = hexNibble(hex[2 * i]);
    const int low = hexNibble(hex[2 * i + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }
  *out_length = length / 2;
  return true;
}

agrotech::FrameFields makeFields(const agrotech::Reading* readings,
                                 uint8_t count, uint16_t key_version = 1,
                                 uint32_t boot_counter = 7,
                                 uint32_t seq = 120) {
  agrotech::FrameFields fields;
  agrotech::initFrameFields(&fields);
  strcpy(fields.coordinator_uid, "CO-7K3M9QXT2A");
  fields.key_version = key_version;
  fields.boot_counter = boot_counter;
  fields.seq = seq;
  fields.reading_count = count;
  for (uint8_t i = 0; i < count; ++i) {
    fields.readings[i] = readings[i];
  }
  return fields;
}

const uint8_t kOfficialSigned[] = {
    0x01, 0x43, 0x4f, 0x2d, 0x37, 0x4b, 0x33, 0x4d, 0x39, 0x51, 0x58, 0x54,
    0x32, 0x41, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x78,
    0x02, 0x00, 0x01, 0x01, 0x09, 0xe6, 0x00, 0x02, 0x02, 0x18, 0xb5};

const uint8_t kOfficialTag[] = {
    0xdd, 0xba, 0xb4, 0x81, 0xe3, 0xb4, 0xf7, 0x1d, 0xcc, 0x93, 0x45, 0x2b,
    0x1d, 0x09, 0x43, 0xad, 0x24, 0x16, 0x3c, 0x59, 0xd6, 0xa3, 0xc2, 0xe6,
    0xf2, 0x7a, 0xd3, 0xeb, 0x6b, 0xab, 0x1a, 0xf5};

const char kOfficialBase64[] =
    "AUNPLTdLM005UVhUMkEAAQAAAAcAAAB4AgABAQnmAAICGLXdurSB47T3HcyTRSsdCUOt"
    "JBY8WdajwubyetPra6sa9Q==";

const char kOfficialFrameHex[] =
    "01434f2d374b334d395158543241000100000007000000780200010109e6000202"
    "18b5ddbab481e3b4f71dcc93452b1d0943ad24163c59d6a3c2e6f27ad3eb6bab1"
    "af5";

void testSha256() {
  uint8_t digest[32];
  uint8_t expected[32];
  size_t expected_length = 0;

  agrotech::Sha256 sha;
  sha.update(reinterpret_cast<const uint8_t*>("abc"), 3);
  sha.finish(digest);
  CHECK(hexToBytes(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      expected, sizeof(expected), &expected_length));
  CHECK(expected_length == 32);
  CHECK(memcmp(digest, expected, 32) == 0);

  agrotech::Sha256 empty;
  empty.finish(digest);
  CHECK(hexToBytes(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      expected, sizeof(expected), &expected_length));
  CHECK(memcmp(digest, expected, 32) == 0);

  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  uint8_t mac[32];
  agrotech::hmacSha256(key, sizeof(key),
                       reinterpret_cast<const uint8_t*>("Hi There"), 8, nullptr,
                       0, mac);
  CHECK(hexToBytes(
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      expected, sizeof(expected), &expected_length));
  CHECK(memcmp(mac, expected, 32) == 0);

  uint8_t long_key[131];
  memset(long_key, 0xaa, sizeof(long_key));
  const char* long_message =
      "Test Using Larger Than Block-Size Key - Hash Key First";
  agrotech::hmacSha256(long_key, sizeof(long_key),
                       reinterpret_cast<const uint8_t*>(long_message),
                       strlen(long_message), nullptr, 0, mac);
  CHECK(hexToBytes(
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
      expected, sizeof(expected), &expected_length));
  CHECK(memcmp(mac, expected, 32) == 0);
}

void testOfficialVector() {
  const agrotech::Reading readings[] = {{1, agrotech::kMetricCodeTemperature,
                                         2534},
                                        {2, agrotech::kMetricCodeRelativeHumidity,
                                         6325}};
  agrotech::FrameFields fields = makeFields(readings, 2, 1, 7, 120);
  uint8_t key[agrotech::kHmacKeyBytes];
  for (size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }

  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                              &frame_length) == agrotech::Status::Ok);
  CHECK(frame_length == 67);

  uint8_t expected_frame[agrotech::kMaxFrameBytes];
  size_t expected_length = 0;
  CHECK(hexToBytes(kOfficialFrameHex, expected_frame, sizeof(expected_frame),
                   &expected_length));
  CHECK(expected_length == frame_length);
  CHECK(memcmp(frame, expected_frame, frame_length) == 0);
  CHECK(memcmp(frame, kOfficialSigned, sizeof(kOfficialSigned)) == 0);
  CHECK(memcmp(frame + sizeof(kOfficialSigned), kOfficialTag,
               sizeof(kOfficialTag)) == 0);

  char base64[agrotech::kBase64FrameMaxBytes + 1];
  CHECK(agrotech::base64Encode(frame, frame_length, base64, sizeof(base64)));
  CHECK(strcmp(base64, kOfficialBase64) == 0);

  agrotech::ParsedFrame parsed;
  CHECK(agrotech::parseFrame(frame, frame_length, &parsed) ==
        agrotech::Status::Ok);
  CHECK(strcmp(parsed.fields.coordinator_uid, "CO-7K3M9QXT2A") == 0);
  CHECK(parsed.fields.key_version == 1);
  CHECK(parsed.fields.boot_counter == 7);
  CHECK(parsed.fields.seq == 120);
  CHECK(parsed.fields.reading_count == 2);
  CHECK(parsed.fields.readings[0].channel == 1);
  CHECK(parsed.fields.readings[0].metric_code ==
        agrotech::kMetricCodeTemperature);
  CHECK(parsed.fields.readings[0].value == 2534);
  CHECK(parsed.fields.readings[1].channel == 2);
  CHECK(parsed.fields.readings[1].metric_code ==
        agrotech::kMetricCodeRelativeHumidity);
  CHECK(parsed.fields.readings[1].value == 6325);
  CHECK(parsed.signed_length == sizeof(kOfficialSigned));
  CHECK(parsed.tag == frame + sizeof(kOfficialSigned));
  CHECK(agrotech::verifyFrameTag(key, frame, frame_length) ==
        agrotech::Status::Ok);
}

void testSentinelVector() {
  const agrotech::Reading readings[] = {
      {1, agrotech::kMetricCodeTemperature, agrotech::kTemperatureInvalidMarker},
      {2, agrotech::kMetricCodeRelativeHumidity,
       agrotech::kHumidityInvalidMarker}};
  agrotech::FrameFields fields = makeFields(readings, 2, 1, 7, 121);
  uint8_t key[agrotech::kHmacKeyBytes];
  for (size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }

  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                              &frame_length) == agrotech::Status::Ok);
  CHECK(frame_length == 67);

  uint8_t expected[agrotech::kMaxFrameBytes];
  size_t expected_length = 0;
  CHECK(hexToBytes(
      "01434f2d374b334d395158543241000100000007000000790200010180000002"
      "02ffff1c9d5c3669b6bb7106aa9fe39ffb04e5498df10a4d9e8a240a74556a5b"
      "c4acb9",
      expected, sizeof(expected), &expected_length));
  CHECK(expected_length == frame_length);
  CHECK(memcmp(frame, expected, frame_length) == 0);

  agrotech::ParsedFrame parsed;
  CHECK(agrotech::parseFrame(frame, frame_length, &parsed) ==
        agrotech::Status::Ok);
  CHECK(parsed.fields.readings[0].value == -32768);
  CHECK(parsed.fields.readings[1].value == 65535);
  CHECK(agrotech::verifyFrameTag(key, frame, frame_length) ==
        agrotech::Status::Ok);
}

void testSingleZeroVector() {
  const agrotech::Reading readings[] = {
      {1, agrotech::kMetricCodeTemperature, 0}};
  agrotech::FrameFields fields = makeFields(readings, 1, 1, 1, 0);
  uint8_t key[agrotech::kHmacKeyBytes];
  for (size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }

  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                              &frame_length) == agrotech::Status::Ok);
  CHECK(frame_length == agrotech::kMinFrameBytes);
  CHECK(frame_length == 62);

  uint8_t expected[agrotech::kMaxFrameBytes];
  size_t expected_length = 0;
  CHECK(hexToBytes(
      "01434f2d374b334d395158543241000100000001000000000100010100006cf2"
      "6b1d1eb1f7156f5751c5fa13ca76078f1202343d97d37da747d494a958e9",
      expected, sizeof(expected), &expected_length));
  CHECK(expected_length == frame_length);
  CHECK(memcmp(frame, expected, frame_length) == 0);
}

void testBigEndianVector() {
  const agrotech::Reading readings[] = {
      {0x0102, agrotech::kMetricCodeTemperature, 0x0304}};
  agrotech::FrameFields fields = makeFields(readings, 1, 0x1234, 0x01020304,
                                            0xa1b2c3d4);
  uint8_t key[agrotech::kHmacKeyBytes];
  for (size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }

  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                              &frame_length) == agrotech::Status::Ok);

  uint8_t expected[agrotech::kMaxFrameBytes];
  size_t expected_length = 0;
  CHECK(hexToBytes(
      "01434f2d374b334d395158543241123401020304a1b2c3d40101020103046477"
      "e709994fe02f3abdc1ec14cd831e720bd23991f24e0796f00c091589e8bb",
      expected, sizeof(expected), &expected_length));
  CHECK(expected_length == frame_length);
  CHECK(memcmp(frame, expected, frame_length) == 0);

  CHECK(frame[14] == 0x12);
  CHECK(frame[15] == 0x34);
  CHECK(frame[16] == 0x01);
  CHECK(frame[17] == 0x02);
  CHECK(frame[18] == 0x03);
  CHECK(frame[19] == 0x04);
  CHECK(frame[20] == 0xa1);
  CHECK(frame[21] == 0xb2);
  CHECK(frame[22] == 0xc3);
  CHECK(frame[23] == 0xd4);
  CHECK(frame[24] == 0x01);
  CHECK(frame[25] == 0x01);
  CHECK(frame[26] == 0x02);
  CHECK(frame[27] == 0x01);
  CHECK(frame[28] == 0x03);
  CHECK(frame[29] == 0x04);
}

void testFrameSizes() {
  CHECK(agrotech::kMinFrameBytes == 62);
  CHECK(agrotech::kMaxFrameBytes == 137);
  CHECK(agrotech::frameSize(1) == 62);
  CHECK(agrotech::frameSize(2) == 67);
  CHECK(agrotech::frameSize(4) == 77);
  CHECK(agrotech::frameSize(16) == 137);

  agrotech::Reading readings[agrotech::kMaxReadingsV1];
  for (size_t i = 0; i < agrotech::kMaxReadingsV1; ++i) {
    readings[i].channel = static_cast<uint16_t>(i + 1);
    readings[i].metric_code = agrotech::kMetricCodeTemperature;
    readings[i].value = 0;
  }

  uint8_t key[agrotech::kHmacKeyBytes];
  memset(key, 0x11, sizeof(key));
  for (uint8_t count = 1; count <= agrotech::kMaxReadingsV1; ++count) {
    agrotech::FrameFields fields = makeFields(readings, count);
    uint8_t frame[agrotech::kMaxFrameBytes];
    size_t frame_length = 0;
    CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                                &frame_length) == agrotech::Status::Ok);
    CHECK(frame_length == agrotech::frameSize(count));
    CHECK(frame_length == 57 + 5 * static_cast<size_t>(count));
  }
}

void testUidValidation() {
  CHECK(agrotech::isValidUid("CO-7K3M9QXT2A", "CO-"));
  CHECK(agrotech::isValidUid("GW-7K3M9QXT2A", "GW-"));
  CHECK(!agrotech::isValidUid("GW-7K3M9QXT2A", "CO-"));
  CHECK(!agrotech::isValidUid("co-7K3M9QXT2A", "CO-"));
  CHECK(!agrotech::isValidUid("CO-7K3M9QXT2", "CO-"));
  CHECK(!agrotech::isValidUid("CO-7K3M9QXT20", "CO-"));
  CHECK(!agrotech::isValidUid("CO-7K3M9QXT2I", "CO-"));
  CHECK(!agrotech::isValidUid("CO-7K3M9QXT2L", "CO-"));
  CHECK(!agrotech::isValidUid(nullptr, "CO-"));
  CHECK(!agrotech::isValidUid("CO-7K3M9QXT2A", nullptr));
}

void testValidation() {
  agrotech::Reading readings[] = {
      {1, agrotech::kMetricCodeTemperature, 2534}};
  agrotech::FrameFields fields = makeFields(readings, 1);
  uint8_t buffer[agrotech::kMaxFrameBytes];
  size_t length = 0;

  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::Ok);
  CHECK(length == 30);

  strcpy(fields.coordinator_uid, "CO-7K3M9QXT2A");
  fields.coordinator_uid[3] = 'a';
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::InvalidUid);
  fields = makeFields(readings, 1);

  fields.key_version = 0;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::InvalidKeyVersion);
  fields.key_version = 1;

  fields.reading_count = 0;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::InvalidReadingCount);
  fields.reading_count = 17;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::InvalidReadingCount);
  fields.reading_count = 1;

  fields.readings[0].channel = 0;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::InvalidChannel);
  fields.readings[0].channel = 1;

  fields.readings[0].metric_code = 9;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::UnknownMetricCode);
  fields.readings[0].metric_code = agrotech::kMetricCodeTemperature;

  fields.readings[0].value = 32768;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::ValueOutOfRange);
  fields.readings[0].value = -32768;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::Ok);

  fields.readings[0].metric_code = agrotech::kMetricCodeRelativeHumidity;
  fields.readings[0].value = 10000;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::Ok);
  fields.readings[0].value = 65535;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::Ok);
  fields.readings[0].value = 10001;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::ValueOutOfRange);
  fields.readings[0].value = 65534;
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::ValueOutOfRange);
  fields.readings[0].metric_code = agrotech::kMetricCodeTemperature;
  fields.readings[0].value = 2534;

  agrotech::Reading duplicate[] = {
      {1, agrotech::kMetricCodeTemperature, 2534},
      {1, agrotech::kMetricCodeRelativeHumidity, 5000}};
  fields = makeFields(duplicate, 2);
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), &length) ==
        agrotech::Status::DuplicateChannel);

  fields = makeFields(readings, 1);
  CHECK(agrotech::buildSignedBytes(fields, buffer, 10, &length) ==
        agrotech::Status::BufferTooSmall);
  CHECK(agrotech::buildSignedBytes(fields, nullptr, sizeof(buffer), &length) ==
        agrotech::Status::InvalidArgument);
  CHECK(agrotech::buildSignedBytes(fields, buffer, sizeof(buffer), nullptr) ==
        agrotech::Status::InvalidArgument);

  uint8_t key[agrotech::kHmacKeyBytes] = {0};
  uint8_t frame[agrotech::kMaxFrameBytes];
  CHECK(agrotech::encodeFrame(fields, key, frame, 10, &length) ==
        agrotech::Status::BufferTooSmall);
}

void testParseRejections() {
  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(hexToBytes(kOfficialFrameHex, frame, sizeof(frame), &frame_length));
  CHECK(frame_length == 67);

  agrotech::ParsedFrame parsed;
  CHECK(agrotech::parseFrame(nullptr, frame_length, &parsed) ==
        agrotech::Status::InvalidArgument);
  CHECK(agrotech::parseFrame(frame, frame_length, nullptr) ==
        agrotech::Status::InvalidArgument);
  CHECK(agrotech::parseFrame(frame, 0, &parsed) ==
        agrotech::Status::Truncated);
  CHECK(agrotech::parseFrame(frame, 10, &parsed) ==
        agrotech::Status::Truncated);
  CHECK(agrotech::parseFrame(frame, frame_length - 1, &parsed) ==
        agrotech::Status::LengthMismatch);

  uint8_t bad[agrotech::kMaxFrameBytes];
  memcpy(bad, frame, frame_length);

  bad[0] = 2;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::UnsupportedVersion);
  memcpy(bad, frame, frame_length);

  bad[24] = 0;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::InvalidReadingCount);
  bad[24] = 17;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::InvalidReadingCount);
  memcpy(bad, frame, frame_length);

  bad[1] = 'c';
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::InvalidUid);
  memcpy(bad, frame, frame_length);

  bad[14] = 0;
  bad[15] = 0;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::InvalidKeyVersion);
  memcpy(bad, frame, frame_length);

  bad[25] = 0;
  bad[26] = 0;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::InvalidChannel);
  memcpy(bad, frame, frame_length);

  bad[27] = 9;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::UnknownMetricCode);
  memcpy(bad, frame, frame_length);

  bad[30] = 0;
  bad[31] = 1;
  CHECK(agrotech::parseFrame(bad, frame_length, &parsed) ==
        agrotech::Status::DuplicateChannel);

  uint8_t key[agrotech::kHmacKeyBytes];
  for (size_t i = 0; i < sizeof(key); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }
  CHECK(agrotech::verifyFrameTag(key, frame, frame_length) ==
        agrotech::Status::Ok);
  memcpy(bad, frame, frame_length);
  bad[frame_length - 1] ^= 0x01;
  CHECK(agrotech::verifyFrameTag(key, bad, frame_length) ==
        agrotech::Status::InvalidHmac);
  memcpy(bad, frame, frame_length);
  bad[30] ^= 0x01;
  CHECK(agrotech::verifyFrameTag(key, bad, frame_length) ==
        agrotech::Status::InvalidHmac);
}

void testCounters() {
  agrotech::CounterState state;

  CHECK(agrotech::planNewBoot(0, &state) == agrotech::Status::Ok);
  CHECK(state.boot_counter == 1);
  CHECK(state.seq == 0);
  CHECK(!state.boot_exhausted);
  CHECK(!state.seq_exhausted);

  CHECK(agrotech::planNewBoot(41, &state) == agrotech::Status::Ok);
  CHECK(state.boot_counter == 42);

  CHECK(agrotech::planNewBoot(0xffffffffu, &state) ==
        agrotech::Status::BootCounterExhausted);
  CHECK(state.boot_exhausted);
  CHECK(state.seq_exhausted);

  CHECK(agrotech::planNewBoot(0, nullptr) ==
        agrotech::Status::InvalidArgument);
  CHECK(agrotech::takeSequence(nullptr, nullptr) ==
        agrotech::Status::InvalidArgument);

  agrotech::CounterState sequence_state;
  CHECK(agrotech::planNewBoot(0, &sequence_state) == agrotech::Status::Ok);
  uint32_t sequence = 99;
  CHECK(agrotech::takeSequence(&sequence_state, &sequence) ==
        agrotech::Status::Ok);
  CHECK(sequence == 0);
  CHECK(agrotech::takeSequence(&sequence_state, &sequence) ==
        agrotech::Status::Ok);
  CHECK(sequence == 1);

  sequence_state.seq = 0xffffffffu;
  sequence_state.seq_exhausted = false;
  CHECK(agrotech::takeSequence(&sequence_state, &sequence) ==
        agrotech::Status::Ok);
  CHECK(sequence == 0xffffffffu);
  CHECK(sequence_state.seq_exhausted);
  CHECK(sequence_state.seq == 0xffffffffu);
  CHECK(agrotech::takeSequence(&sequence_state, &sequence) ==
        agrotech::Status::SequenceExhausted);

  sequence_state.boot_exhausted = true;
  CHECK(agrotech::takeSequence(&sequence_state, &sequence) ==
        agrotech::Status::BootCounterExhausted);
}

void testBase64() {
  CHECK(agrotech::base64EncodedLength(0) == 0);
  CHECK(agrotech::base64EncodedLength(1) == 4);
  CHECK(agrotech::base64EncodedLength(2) == 4);
  CHECK(agrotech::base64EncodedLength(3) == 4);
  CHECK(agrotech::base64EncodedLength(4) == 8);

  char out[16];
  const uint8_t ff = 0xff;
  CHECK(agrotech::base64Encode(&ff, 1, out, sizeof(out)));
  CHECK(strcmp(out, "/w==") == 0);

  const uint8_t two[] = {0xff, 0xff};
  CHECK(agrotech::base64Encode(two, 2, out, sizeof(out)));
  CHECK(strcmp(out, "//8=") == 0);

  CHECK(agrotech::base64Encode(nullptr, 0, out, sizeof(out)));
  CHECK(strcmp(out, "") == 0);
  CHECK(!agrotech::base64Encode(&ff, 1, out, 4));
  CHECK(!agrotech::base64Encode(nullptr, 1, out, sizeof(out)));
  CHECK(!agrotech::base64Encode(&ff, 1, nullptr, sizeof(out)));
}

void testMqttPayload() {
  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(hexToBytes(kOfficialFrameHex, frame, sizeof(frame), &frame_length));

  agrotech::MqttPayloadInput input;
  input.frame = frame;
  input.frame_length = frame_length;
  input.lora_rx_at_iso = "2026-11-12T14:03:22Z";
  input.clock_synced = true;
  input.buffered = false;
  input.rssi = -90;
  input.snr = 7.5f;

  char payload[agrotech::kMqttPayloadMaxBytes];
  CHECK(agrotech::buildMqttPayload(input, payload, sizeof(payload)));
  const char* expected =
      "{\"v\":1,\"frame\":\"AUNPLTdLM005UVhUMkEAAQAAAAcAAAB4AgABAQnmAAICGLX"
      "durSB47T3HcyTRSsdCUOtJBY8WdajwubyetPra6sa9Q==\",\"lora_rx_at\":"
      "\"2026-11-12T14:03:22Z\",\"clock_synced\":true,\"buffered\":false,"
      "\"rssi\":-90,\"snr\":7.50}";
  CHECK(strcmp(payload, expected) == 0);
  CHECK(strstr(payload, "gateway_uid") == nullptr);
  CHECK(strstr(payload, "farm_id") == nullptr);

  input.lora_rx_at_iso = nullptr;
  input.clock_synced = true;
  CHECK(agrotech::buildMqttPayload(input, payload, sizeof(payload)));
  CHECK(strstr(payload, "\"lora_rx_at\":null") != nullptr);
  CHECK(strstr(payload, "\"clock_synced\":false") != nullptr);

  input.lora_rx_at_iso = "2026-11-12T14:03:22Z";
  input.clock_synced = false;
  CHECK(agrotech::buildMqttPayload(input, payload, sizeof(payload)));
  CHECK(strstr(payload, "\"lora_rx_at\":null") != nullptr);
  CHECK(strstr(payload, "\"clock_synced\":false") != nullptr);

  input.clock_synced = true;
  input.buffered = true;
  CHECK(agrotech::buildMqttPayload(input, payload, sizeof(payload)));
  CHECK(strstr(payload, "\"buffered\":true") != nullptr);

  input.buffered = false;
  input.frame_length = agrotech::kMaxFrameBytes + 1;
  CHECK(!agrotech::buildMqttPayload(input, payload, sizeof(payload)));
  input.frame_length = frame_length;
  CHECK(!agrotech::buildMqttPayload(input, payload, 10));
  CHECK(!agrotech::buildMqttPayload(input, nullptr, sizeof(payload)));
}

void testCoordinatorSensorMath() {
  int32_t value = 0;
  CHECK(coordinator_sensor::scaleX100(25.34f, -4000, 8500, &value));
  CHECK(value == 2534);
  CHECK(coordinator_sensor::scaleX100(25.346f, -4000, 8500, &value));
  CHECK(value == 2535);  // rounding to nearest hundredth
  CHECK(coordinator_sensor::scaleX100(-3.25f, -4000, 8500, &value));
  CHECK(value == -325);
  CHECK(coordinator_sensor::scaleX100(-0.126f, -4000, 8500, &value));
  CHECK(value == -13);  // lroundf rounds halfway away from zero
  CHECK(coordinator_sensor::scaleX100(63.25f, 0, 10000, &value));
  CHECK(value == 6325);  // relative humidity ×100
  CHECK(coordinator_sensor::scaleX100(85.0f, -4000, 8500, &value));
  CHECK(value == 8500);
  CHECK(!coordinator_sensor::scaleX100(NAN, -4000, 8500, &value));
  CHECK(!coordinator_sensor::scaleX100(INFINITY, -4000, 8500, &value));
  CHECK(!coordinator_sensor::scaleX100(-40.01f, -4000, 8500, &value));
  CHECK(!coordinator_sensor::scaleX100(85.01f, -4000, 8500, &value));
  CHECK(!coordinator_sensor::scaleX100(20.0f, 0, 10000, nullptr));

  CHECK(coordinator_sensor::selectSource(true, true) ==
        coordinator_sensor::Source::Bme680);
  CHECK(coordinator_sensor::selectSource(true, false) ==
        coordinator_sensor::Source::Bme680);
  CHECK(coordinator_sensor::selectSource(false, true) ==
        coordinator_sensor::Source::Dht11);
  CHECK(coordinator_sensor::selectSource(false, false) ==
        coordinator_sensor::Source::None);

  agrotech::FrameFields fields;
  agrotech::initFrameFields(&fields);
  strcpy(fields.coordinator_uid, "CO-7K3M9QXT2A");
  fields.key_version = 1;
  fields.reading_count = 2;
  fields.readings[0].channel = 1;
  fields.readings[0].metric_code = agrotech::kMetricCodeTemperature;
  fields.readings[0].value = 2534;  // previous cycle's cached value
  fields.readings[1].channel = 2;
  fields.readings[1].metric_code = agrotech::kMetricCodeRelativeHumidity;
  fields.readings[1].value = 6325;
  coordinator_sensor::clearReadings(&fields);
  CHECK(fields.reading_count == 0);
  CHECK(coordinator_sensor::selectSource(false, false) ==
        coordinator_sensor::Source::None);
  uint8_t key[agrotech::kHmacKeyBytes] = {0};
  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                              &frame_length) ==
        agrotech::Status::InvalidReadingCount);
  coordinator_sensor::clearReadings(nullptr);
}

// ---------------------------------------------------------------------------
// BUILD-IOT-SENSOR-FAILOVER-05: pure failover state machine (CoordinatorSensorMath.h)
// ---------------------------------------------------------------------------

using coordinator_sensor::FailoverCounters;
using coordinator_sensor::FailoverDecision;
using coordinator_sensor::FailoverState;
using coordinator_sensor::Source;

const uint8_t kF = COORDINATOR_FAILOVER_FAIL_THRESHOLD;
const uint8_t kR = COORDINATOR_FAILOVER_RECOVERY_THRESHOLD;
const uint8_t kK = COORDINATOR_BME680_REINIT_INTERVAL_CYCLES;

struct FailoverStep {
  bool bme;
  bool dht;
  FailoverState state;
  Source publish;
  uint8_t fail;
  uint8_t ok;
  bool degraded;
};

void runFailoverSteps(const char* name, const FailoverStep* steps,
                      size_t count) {
  FailoverCounters counters;
  coordinator_sensor::initFailoverCounters(&counters);
  for (size_t i = 0; i < count; ++i) {
    const FailoverDecision d = coordinator_sensor::failoverStep(
        &counters, steps[i].bme, steps[i].dht, kF, kR);
    const bool ok = d.state == steps[i].state &&
                    d.publish == steps[i].publish &&
                    counters.fail == steps[i].fail &&
                    counters.ok == steps[i].ok &&
                    counters.degraded == steps[i].degraded;
    if (!ok) printf("FAIL %s step %u\n", name, static_cast<unsigned>(i + 1));
    CHECK(ok);
  }
}

#define STEPS(name, array) \
  runFailoverSteps(name, array, sizeof(array) / sizeof(array[0]))

void testFailoverParameters() {
  CHECK(kF == 3 && kR == 3 && kK == 5);
  CHECK(COORDINATOR_BME680_TEMPERATURE_CHANNEL == 1);
  CHECK(COORDINATOR_BME680_HUMIDITY_CHANNEL == 2);
  CHECK(COORDINATOR_DHT11_TEMPERATURE_CHANNEL == 3);
  CHECK(COORDINATOR_DHT11_HUMIDITY_CHANNEL == 4);
  FailoverCounters c;
  c.fail = 9;
  c.ok = 9;
  c.degraded = true;
  coordinator_sensor::initFailoverCounters(&c);  // D-1
  CHECK(c.fail == 0 && c.ok == 0 && !c.degraded);
}

void testFailoverStateMachine() {
  const FailoverStep available[] = {  // case 1
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false},
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false}};
  STEPS("boot with BME680", available);

  const FailoverStep unavailable[] = {  // cases 2, 3, 4
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 2, 0, false},
      {false, true, FailoverState::Respaldo, Source::Dht11, 3, 0, true},
      {false, true, FailoverState::Respaldo, Source::Dht11, 3, 0, true}};
  STEPS("boot without BME680", unavailable);

  const FailoverStep suspicion_recovery[] = {  // case 5 (D-2)
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 2, 0, false},
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false}};
  STEPS("SOSPECHA back to PRINCIPAL", suspicion_recovery);

  const FailoverStep backup_recovery[] = {  // cases 6, 7, 8 (D-4)
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 2, 0, false},
      {false, true, FailoverState::Respaldo, Source::Dht11, 3, 0, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 1, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 2, true},
      {true, true, FailoverState::Principal, Source::Bme680, 0, 0, false}};
  STEPS("RESPALDO recovery", backup_recovery);

  const FailoverStep recovery_without_dht[] = {  // case 9 (D-3)
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 2, 0, false},
      {false, true, FailoverState::Respaldo, Source::Dht11, 3, 0, true},
      {true, false, FailoverState::Recuperacion, Source::Bme680, 0, 1, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 2, true},
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false}};
  STEPS("RECUPERACION with DHT11 failing", recovery_without_dht);

  const FailoverStep both_fail[] = {  // case 10
      {false, false, FailoverState::SinSensor, Source::None, 1, 0, false},
      {false, false, FailoverState::SinSensor, Source::None, 2, 0, false},
      {false, false, FailoverState::SinSensor, Source::None, 3, 0, true},
      {false, true, FailoverState::Respaldo, Source::Dht11, 3, 0, true},
      {false, false, FailoverState::SinSensor, Source::None, 3, 0, true},
      {true, false, FailoverState::Recuperacion, Source::Bme680, 0, 1, true},
      {false, false, FailoverState::SinSensor, Source::None, 1, 0, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 1, true}};
  STEPS("SIN_SENSOR keeps a coherent state", both_fail);

  const FailoverStep both_fail_from_principal[] = {
      {false, false, FailoverState::SinSensor, Source::None, 1, 0, false},
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false}};
  STEPS("SIN_SENSOR then BME680", both_fail_from_principal);

  const FailoverStep intermittent[] = {  // case 11
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 2, 0, false},
      {true, false, FailoverState::Principal, Source::Bme680, 0, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 1, 0, false},
      {false, true, FailoverState::Sospecha, Source::Dht11, 2, 0, false},
      {false, true, FailoverState::Respaldo, Source::Dht11, 3, 0, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 1, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 2, true},
      {false, true, FailoverState::Respaldo, Source::Dht11, 1, 0, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 1, true},
      {true, true, FailoverState::Recuperacion, Source::Dht11, 0, 2, true},
      {true, true, FailoverState::Principal, Source::Bme680, 0, 0, false}};
  STEPS("intermittent failures do not accumulate", intermittent);

  // Saturation and one-shot flags.
  FailoverCounters c;
  coordinator_sensor::initFailoverCounters(&c);
  int entered = 0;
  for (int i = 0; i < 50; ++i) {
    const FailoverDecision d =
        coordinator_sensor::failoverStep(&c, false, true, kF, kR);
    if (d.entered_degraded) ++entered;
  }
  CHECK(c.fail == kF && entered == 1);
  int recovered = 0;
  for (int i = 0; i < 3; ++i) {
    const FailoverDecision d =
        coordinator_sensor::failoverStep(&c, true, true, kF, kR);
    if (d.recovered) {
      ++recovered;
      CHECK(i == 2 && d.state == FailoverState::Principal &&
            d.publish == Source::Bme680);
    }
  }
  CHECK(recovered == 1 && !c.degraded && c.ok == 0);

  // DHT11 is read only when needed.
  coordinator_sensor::initFailoverCounters(&c);
  CHECK(!coordinator_sensor::failoverNeedsDht(c, true));
  CHECK(coordinator_sensor::failoverNeedsDht(c, false));
  c.degraded = true;
  CHECK(coordinator_sensor::failoverNeedsDht(c, true));

  // Defensive inputs never publish.
  CHECK(coordinator_sensor::failoverStep(nullptr, true, true, kF, kR).publish ==
        Source::None);
  CHECK(coordinator_sensor::failoverStep(&c, true, true, 0, kR).publish ==
        Source::None);
}

void testFailoverInvariants() {
  // Deterministic pseudo-random sequences: invariants of the approved design.
  FailoverCounters c;
  coordinator_sensor::initFailoverCounters(&c);
  uint32_t lcg = 12345u;
  int shadow_fail_run = 0;
  int shadow_ok_run = 0;
  bool shadow_degraded = false;
  bool ok = true;
  for (int i = 0; i < 100000; ++i) {
    lcg = lcg * 1103515245u + 12345u;
    const bool bme = ((lcg >> 16) % 100) < 60;
    const bool dht = ((lcg >> 8) % 100) < 80;
    const FailoverDecision d =
        coordinator_sensor::failoverStep(&c, bme, dht, kF, kR);
    if (bme) {
      shadow_fail_run = 0;
      if (shadow_degraded) {
        ++shadow_ok_run;
        if (shadow_ok_run >= kR) {
          shadow_degraded = false;
          shadow_ok_run = 0;
        }
      }
    } else {
      shadow_ok_run = 0;
      ++shadow_fail_run;
      if (shadow_fail_run >= kF) shadow_degraded = true;
    }
    ok = ok && c.degraded == shadow_degraded;
    ok = ok && c.fail <= kF && c.ok < kR;
    ok = ok && (d.publish != Source::Bme680 || bme);
    ok = ok && (d.publish != Source::Dht11 || dht);
    ok = ok && ((d.publish == Source::None) == (d.state == FailoverState::SinSensor));
    ok = ok && ((d.state == FailoverState::Principal) == (bme && !c.degraded));
    ok = ok && (d.state != FailoverState::Principal || d.publish == Source::Bme680);
    ok = ok && (d.state != FailoverState::Sospecha || (!c.degraded && !bme));
    ok = ok && (d.state != FailoverState::Respaldo || (c.degraded && !bme));
    ok = ok && (d.state != FailoverState::Recuperacion || (c.degraded && bme));
    ok = ok && (d.state != FailoverState::Recuperacion ||
                d.publish == (dht ? Source::Dht11 : Source::Bme680));
    if (!ok) {
      printf("FAIL failover invariant at cycle %d\n", i);
      break;
    }
  }
  CHECK(ok);
}

void testReinitTimer() {
  // Case 12: boot attempt is cycle 0, retries at cycles 5, 10, 15, 20.
  coordinator_sensor::ReinitTimer timer = {0};
  coordinator_sensor::reinitAfterAttempt(&timer, kK);
  int attempts[8];
  int count = 0;
  for (int cycle = 1; cycle <= 20; ++cycle) {
    if (coordinator_sensor::reinitDue(&timer, kK) && count < 8)
      attempts[count++] = cycle;
  }
  CHECK(count == 4 && attempts[0] == 5 && attempts[1] == 10 &&
        attempts[2] == 15 && attempts[3] == 20);

  // Operational failure: next-cycle attempt, then every K cycles.
  coordinator_sensor::reinitNextCycle(&timer);
  count = 0;
  for (int cycle = 1; cycle <= 11; ++cycle) {
    if (coordinator_sensor::reinitDue(&timer, kK) && count < 8)
      attempts[count++] = cycle;
  }
  CHECK(count == 3 && attempts[0] == 1 && attempts[1] == 6 &&
        attempts[2] == 11);

  // Never two attempts in consecutive cycles.
  coordinator_sensor::reinitNextCycle(&timer);
  bool previous = false;
  bool consecutive = false;
  for (int cycle = 0; cycle < 200; ++cycle) {
    const bool due = coordinator_sensor::reinitDue(&timer, kK);
    consecutive = consecutive || (due && previous);
    previous = due;
  }
  CHECK(!consecutive);
  CHECK(!coordinator_sensor::reinitDue(nullptr, kK));
  CHECK(!coordinator_sensor::reinitDue(&timer, 0));
}

void testFailoverNamesAndReasons() {
  CHECK(strcmp(coordinator_sensor::failoverStateName(FailoverState::Principal),
               "PRINCIPAL") == 0);
  CHECK(strcmp(coordinator_sensor::failoverStateName(FailoverState::Recuperacion),
               "RECUPERACION") == 0);
  CHECK(strcmp(coordinator_sensor::failoverStateName(FailoverState::SinSensor),
               "SIN_SENSOR") == 0);
  FailoverCounters c;
  coordinator_sensor::initFailoverCounters(&c);
  FailoverDecision d = coordinator_sensor::failoverStep(&c, false, true, kF, kR);
  CHECK(strcmp(coordinator_sensor::failoverTransitionReason(
                   FailoverState::Principal, Source::Bme680, d),
               "bme_fail") == 0);
  coordinator_sensor::failoverStep(&c, false, true, kF, kR);
  d = coordinator_sensor::failoverStep(&c, false, true, kF, kR);
  CHECK(strcmp(coordinator_sensor::failoverTransitionReason(
                   FailoverState::Sospecha, Source::Dht11, d),
               "bme_degraded") == 0);
  d = coordinator_sensor::failoverStep(&c, true, false, kF, kR);
  CHECK(strcmp(coordinator_sensor::failoverTransitionReason(
                   FailoverState::Respaldo, Source::Dht11, d),
               "bme_recovering") == 0);
  const FailoverDecision dht_back =
      coordinator_sensor::failoverStep(&c, true, true, kF, kR);
  CHECK(strcmp(coordinator_sensor::failoverTransitionReason(
                   FailoverState::Recuperacion, Source::Bme680, dht_back),
               "dht_restored") == 0);
  d = coordinator_sensor::failoverStep(&c, true, true, kF, kR);
  CHECK(strcmp(coordinator_sensor::failoverTransitionReason(
                   FailoverState::Recuperacion, Source::Dht11, d),
               "bme_recovered") == 0);
  d = coordinator_sensor::failoverStep(&c, false, false, kF, kR);
  CHECK(strcmp(coordinator_sensor::failoverTransitionReason(
                   FailoverState::Principal, Source::Bme680, d),
               "both_failed") == 0);
}

// DHT11 backup pair on channels 3/4 (test_vectors/dht_channels_vector.json,
// generated by the independent Python reference in test_protocol.py).
const char kDhtChannelsFrameHex[] =
    "01434f2d374b334d39515854324100010000000700000079020003010bc20004021b94"
    "f87e2816cca31046c73198e0068745ec5f959caa5cc7e85e119f7c429f9a2bd7";

void testDhtChannelsVector() {
  const agrotech::Reading readings[] = {
      {COORDINATOR_DHT11_TEMPERATURE_CHANNEL, agrotech::kMetricCodeTemperature,
       3010},
      {COORDINATOR_DHT11_HUMIDITY_CHANNEL, agrotech::kMetricCodeRelativeHumidity,
       7060}};
  agrotech::FrameFields fields = makeFields(readings, 2, 1, 7, 121);
  uint8_t key[agrotech::kHmacKeyBytes];
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i);
  uint8_t frame[agrotech::kMaxFrameBytes];
  size_t frame_length = 0;
  CHECK(agrotech::encodeFrame(fields, key, frame, sizeof(frame),
                              &frame_length) == agrotech::Status::Ok);
  uint8_t expected[agrotech::kMaxFrameBytes];
  size_t expected_length = 0;
  CHECK(hexToBytes(kDhtChannelsFrameHex, expected, sizeof(expected),
                   &expected_length));
  CHECK(frame_length == 67 && expected_length == frame_length);
  CHECK(memcmp(frame, expected, frame_length) == 0);

  agrotech::ParsedFrame parsed;
  CHECK(agrotech::parseFrame(frame, frame_length, &parsed) ==
        agrotech::Status::Ok);  // gateway-side structural parser
  CHECK(parsed.fields.readings[0].channel == 3 &&
        parsed.fields.readings[0].metric_code == agrotech::kMetricCodeTemperature &&
        parsed.fields.readings[0].value == 3010);
  CHECK(parsed.fields.readings[1].channel == 4 &&
        parsed.fields.readings[1].metric_code ==
            agrotech::kMetricCodeRelativeHumidity &&
        parsed.fields.readings[1].value == 7060);
  CHECK(agrotech::verifyFrameTag(key, frame, frame_length) ==
        agrotech::Status::Ok);
  frame[26] ^= 0x01;  // tamper with the channel: HMAC must fail
  CHECK(agrotech::verifyFrameTag(key, frame, frame_length) ==
        agrotech::Status::InvalidHmac);

  // BME680 pair keeps channels 1/2 (official vector) and v1 rules still apply.
  const agrotech::Reading repeated[] = {
      {3, agrotech::kMetricCodeTemperature, 3010},
      {3, agrotech::kMetricCodeRelativeHumidity, 7060}};
  agrotech::FrameFields bad = makeFields(repeated, 2, 1, 7, 121);
  CHECK(agrotech::encodeFrame(bad, key, frame, sizeof(frame), &frame_length) ==
        agrotech::Status::DuplicateChannel);
}

}  // namespace

int main() {
  testSha256();
  testOfficialVector();
  testSentinelVector();
  testSingleZeroVector();
  testBigEndianVector();
  testFrameSizes();
  testUidValidation();
  testValidation();
  testParseRejections();
  testCounters();
  testBase64();
  testMqttPayload();
  testCoordinatorSensorMath();
  testFailoverParameters();
  testFailoverStateMachine();
  testFailoverInvariants();
  testReinitTimer();
  testFailoverNamesAndReasons();
  testDhtChannelsVector();

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
