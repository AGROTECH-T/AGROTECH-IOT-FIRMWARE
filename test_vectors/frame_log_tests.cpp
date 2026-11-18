// MQTT-QUEUE-PERSISTENCE-05 - host tests for gateway/GatewayFrameLog.
//
// Runs the pure persistence core against a simulated NOR flash (program ANDs
// bits, erase sets 0xFF) with fault injection: power cut after N programmed
// bytes, interrupted erase, program/read/erase driver errors and corrupted
// program results. No hardware is involved.
//
// Build (host C++17):
//   zig c++ -std=c++17 -Wall -Wextra -I shared/src -I gateway
//     test_vectors/frame_log_tests.cpp gateway/GatewayFrameLog.cpp -o frame_log_tests

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "GatewayFrameLog.h"

using gateway_frame_log::FlashRegion;
using gateway_frame_log::FrameLog;
using gateway_frame_log::LiveFrame;
using gateway_frame_log::OpenResult;
using gateway_frame_log::Status;
using gateway_frame_log::kSectorSize;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_test = "";

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      printf("  FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond);      \
    }                                                                            \
  } while (0)

// ---- simulated NOR flash ----------------------------------------------------

class SimFlash : public FlashRegion {
 public:
  explicit SimFlash(uint32_t sectors) : data(sectors * kSectorSize, 0xFF) {}

  std::vector<uint8_t> data;
  long program_budget = -1;   // bytes still programmable before a power cut
  long erase_cut_bytes = -1;  // next erase stops after N bytes, then power cut
  int fail_programs = 0;      // driver error, nothing written
  long fail_after_bytes = -1; // next program writes N bytes then errors (no cut)
  int corrupt_programs = 0;   // next program clears one extra bit (verify fails)
  int fail_reads = 0;
  int fail_erases = 0;
  bool dead = false;
  long programmed = 0;
  int programs = 0;
  int erases = 0;

  void powerOn() {
    dead = false;
    program_budget = -1;
    erase_cut_bytes = -1;
  }

  uint32_t size() const override { return static_cast<uint32_t>(data.size()); }

  bool read(uint32_t offset, uint8_t* out, uint32_t length) override {
    if (dead) return false;
    if (fail_reads > 0) {
      --fail_reads;
      return false;
    }
    if (offset + length > data.size()) return false;
    memcpy(out, &data[offset], length);
    return true;
  }

  bool program(uint32_t offset, const uint8_t* src, uint32_t length) override {
    if (dead) return false;
    if (offset + length > data.size()) return false;
    if (fail_programs > 0) {
      --fail_programs;
      return false;
    }
    ++programs;
    const bool corrupt = corrupt_programs > 0;
    if (corrupt) --corrupt_programs;
    // Index of the first non-zero source byte: clearing one of its set bits
    // always makes the programmed data differ from the source.
    uint32_t corrupt_at = length;
    for (uint32_t i = 0; corrupt && i < length; ++i) {
      if (src[i] != 0) {
        corrupt_at = i;
        break;
      }
    }
    for (uint32_t i = 0; i < length; ++i) {
      if (program_budget == 0) {
        dead = true;  // power cut in the middle of the write
        return false;
      }
      if (fail_after_bytes == 0) {
        fail_after_bytes = -1;
        return false;
      }
      if (program_budget > 0) --program_budget;
      if (fail_after_bytes > 0) --fail_after_bytes;
      uint8_t value = src[i];
      if (i == corrupt_at) value = static_cast<uint8_t>(value & (value - 1));
      data[offset + i] &= value;
      ++programmed;
    }
    return true;
  }

  bool eraseSector(uint32_t sector) override {
    if (dead) return false;
    const size_t base = static_cast<size_t>(sector) * kSectorSize;
    if (base + kSectorSize > data.size()) return false;
    if (fail_erases > 0) {
      --fail_erases;
      return false;
    }
    ++erases;
    if (erase_cut_bytes >= 0) {
      for (long i = 0; i < erase_cut_bytes && i < static_cast<long>(kSectorSize); ++i) {
        data[base + i] = 0xFF;
      }
      dead = true;
      return false;
    }
    memset(&data[base], 0xFF, kSectorSize);
    return true;
  }
};

// ---- helpers ------------------------------------------------------------------

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Deterministic frame #n (independent of the sequence number it receives).
LiveFrame frameFor(uint32_t n) {
  LiveFrame f;
  memset(&f, 0, sizeof(f));
  f.length = static_cast<uint16_t>(1 + (n * 37) % gateway_frame_log::kMaxFrameBytes);
  for (uint16_t i = 0; i < f.length; ++i) f.bytes[i] = static_cast<uint8_t>(n * 31 + i * 7 + 1);
  f.rssi = static_cast<int16_t>(-30 - static_cast<int>(n % 90));
  f.snr = -7.25f + 0.5f * static_cast<float>(n % 40);
  f.rx_time = 1700000000LL + static_cast<int64_t>(n) * 61;
  f.time_synced_at_rx = (n % 2) == 0;
  f.buffered_hint = (n % 3) == 0;
  return f;
}

bool sameFrame(const LiveFrame& a, const LiveFrame& b) {
  uint32_t sa = 0, sb = 0;
  memcpy(&sa, &a.snr, 4);
  memcpy(&sb, &b.snr, 4);
  return a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0 && a.rssi == b.rssi &&
         sa == sb && a.rx_time == b.rx_time && a.time_synced_at_rx == b.time_synced_at_rx;
}

// Offset of the record of `type` carrying `seq` (scans 16-byte slots).
long findRecord(const SimFlash& flash, uint8_t type, uint32_t seq, int occurrence = 0) {
  for (size_t o = 0; o + 32 <= flash.data.size(); o += 16) {
    if (o % kSectorSize == 0) continue;  // sector header slot
    const uint8_t* p = &flash.data[o];
    if (p[0] == 'A' && p[1] == 'R' && p[2] == type && p[3] == 1 && get32(p + 8) == seq) {
      if (occurrence-- == 0) return static_cast<long>(o);
    }
  }
  return -1;
}

// All live frames in FIFO order, read from a throw-away copy of the flash.
std::vector<LiveFrame> snapshotLive(const SimFlash& flash) {
  SimFlash copy = flash;
  copy.powerOn();
  std::unique_ptr<FrameLog> log(new FrameLog());
  std::vector<LiveFrame> out;
  if (log->open(&copy, 1) != OpenResult::Recovered) return out;
  LiveFrame f;
  while (log->peekHead(&f)) {
    out.push_back(f);
    // Like mqttTask: run maintenance when the tail needs a prepared sector.
    Status status = log->acknowledge(f.seq);
    for (int i = 0; status != Status::Ok && i < 8; ++i) {
      log->maintain();
      status = log->acknowledge(f.seq);
    }
    if (status != Status::Ok) break;
  }
  return out;
}

std::unique_ptr<SimFlash> freshInitialized(uint32_t sectors) {
  std::unique_ptr<SimFlash> flash(new SimFlash(sectors));
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 0x1234ABCDu);
  return flash;
}

// ---- model used by the power-cut tests ----------------------------------------

struct Model {
  std::map<uint32_t, uint32_t> accepted;  // seq -> frame number
  std::set<uint32_t> acked;
  std::map<uint32_t, bool> frozen_ok;     // FREEZE returned Ok (2 copies)
  uint32_t max_seq = 0;
  uint32_t next_frame = 0;
  // Operation interrupted by the power cut. Its record may still be complete
  // (e.g. the missing last bytes were 0xFF), so after the reboot it may or may
  // not have taken effect: both outcomes are accepted, nothing else is.
  enum { kNone, kAppend, kAck } inflight = kNone;
  uint32_t inflight_seq = 0;    // kAck: frame being confirmed
  uint32_t inflight_frame = 0;  // kAppend: frame number being stored
};

// mqttTask-like workload: append, maintain, freeze head, ack head.
void runWorkload(FrameLog& log, SimFlash& flash, Model& model, int steps, uint32_t seed) {
  uint32_t r = seed;
  for (int i = 0; i < steps && !flash.dead; ++i) {
    r = r * 1103515245u + 12345u;
    const uint32_t op = (r >> 16) % 10;
    model.inflight = Model::kNone;
    if (op < 4) {
      const uint32_t n = model.next_frame++;
      model.inflight = Model::kAppend;
      model.inflight_frame = n;
      uint32_t seq = 0;
      if (log.append(frameFor(n), &seq) == Status::Ok) {
        model.accepted[seq] = n;
        if (seq > model.max_seq) model.max_seq = seq;
      }
    } else if (op < 6) {
      log.maintain();
    } else {
      LiveFrame head;
      if (!log.peekHead(&head)) continue;
      if (!head.frozen || head.freeze_copies < 2) {
        const bool buffered = head.frozen ? head.buffered : ((r >> 8) & 1u) != 0;
        if (log.freeze(head.seq, buffered) == Status::Ok) model.frozen_ok[head.seq] = buffered;
      } else {
        model.inflight = Model::kAck;
        model.inflight_seq = head.seq;
        if (log.acknowledge(head.seq) == Status::Ok) model.acked.insert(head.seq);
      }
    }
    if (!flash.dead) model.inflight = Model::kNone;
  }
}

// Invariants after a reboot. Returns false if the store could not be opened.
bool verifyRecovered(SimFlash& flash, Model& model, std::unique_ptr<FrameLog>& out) {
  flash.powerOn();
  out.reset(new FrameLog());
  const OpenResult result = out->open(&flash, 99);
  CHECK(result == OpenResult::Recovered);
  if (result != OpenResult::Recovered) return false;
  const std::vector<LiveFrame> live = snapshotLive(flash);
  // Resolve the interrupted operation from what actually became durable.
  if (model.inflight == Model::kAck && !model.acked.count(model.inflight_seq)) {
    bool still_live = false;
    for (const LiveFrame& f : live) still_live = still_live || f.seq == model.inflight_seq;
    if (!still_live) model.acked.insert(model.inflight_seq);  // ACK completed
  } else if (model.inflight == Model::kAppend && !live.empty()) {
    const LiveFrame& last = live.back();
    if (!model.accepted.count(last.seq) && last.seq > model.max_seq &&
        sameFrame(last, frameFor(model.inflight_frame))) {
      model.accepted[last.seq] = model.inflight_frame;  // FRAME completed
      model.max_seq = last.seq;
    }
  }
  model.inflight = Model::kNone;
  std::vector<uint32_t> expected;
  for (const auto& kv : model.accepted) {
    if (!model.acked.count(kv.first)) expected.push_back(kv.first);
  }
  CHECK(live.size() == expected.size());
  CHECK(out->liveCount() == expected.size());
  if (live.size() != expected.size() && g_failures < 4) {
    printf("    expected:");
    for (uint32_t s : expected) printf(" %u", s);
    printf("\n    recovered(%u):", static_cast<unsigned>(out->liveCount()));
    for (const LiveFrame& f : live) printf(" %u", f.seq);
    printf("\n");
  }
  for (size_t i = 0; i < live.size() && i < expected.size(); ++i) {
    CHECK(live[i].seq == expected[i]);  // FIFO, no loss, no resurrection
    const auto it = model.accepted.find(live[i].seq);
    if (it != model.accepted.end()) CHECK(sameFrame(live[i], frameFor(it->second)));
    CHECK(!live[i].freeze_unknown);     // torn writes never block a frame
    const auto fz = model.frozen_ok.find(live[i].seq);
    if (fz != model.frozen_ok.end()) {
      CHECK(live[i].frozen && live[i].buffered == fz->second && live[i].freeze_copies == 2);
    }
  }
  CHECK(out->counters().freeze_unknown == 0);
  return true;
}

// ---- tests ----------------------------------------------------------------------

void testInitVirgin() {
  g_test = "init_virgin";
  SimFlash flash(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(&flash, 7) == OpenResult::Initialized);
  CHECK(log->ready() && log->liveCount() == 0);
  CHECK(get32(&flash.data[0]) == 0x31514741u);  // AGQ1 header written
  CHECK(flash.data[32] == 'A' && flash.data[34] == 5);  // INIT record
  std::unique_ptr<FrameLog> again(new FrameLog());
  const int programs = flash.programs;
  CHECK(again->open(&flash, 8) == OpenResult::Recovered);
  CHECK(flash.programs == programs);  // a normal boot writes nothing
}

void testForeignDataRejected() {
  g_test = "foreign_data_rejected";
  SimFlash flash(6);
  flash.data[3 * kSectorSize + 1000] = 0x00;  // one unknown byte anywhere
  const std::vector<uint8_t> before = flash.data;
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(&flash, 7) == OpenResult::ForeignData);
  CHECK(!log->ready());
  CHECK(flash.data == before && flash.programs == 0 && flash.erases == 0);
  CHECK(log->append(frameFor(1), nullptr) == Status::Unavailable);

  SimFlash dirty_header(6);
  dirty_header.data[2 * kSectorSize] = 0x12;  // programmed header, not ours
  std::unique_ptr<FrameLog> log2(new FrameLog());
  CHECK(log2->open(&dirty_header, 7) == OpenResult::ForeignData);
  CHECK(dirty_header.programs == 0 && dirty_header.erases == 0);
}

void testIncompatibleVersion() {
  g_test = "incompatible_version";
  auto flash = freshInitialized(6);
  put32(&flash->data[4], (32u << 16) | 2u);  // format_version 2, header_size 32
  put32(&flash->data[28], crc32(&flash->data[0], 28));
  const std::vector<uint8_t> before = flash->data;
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(flash.get(), 7) == OpenResult::Incompatible);
  CHECK(flash->data == before);
}

void testBadGeometry() {
  g_test = "bad_geometry";
  SimFlash small(3);
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(&small, 7) == OpenResult::BadRegion);
  CHECK(small.programs == 0);
}

void testFrameRoundTripAndFifo() {
  g_test = "frame_roundtrip_fifo";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(flash.get(), 1) == OpenResult::Recovered);
  for (uint32_t n = 0; n < 5; ++n) {
    uint32_t seq = 0;
    CHECK(log->append(frameFor(n), &seq) == Status::Ok);
    CHECK(seq == n + 1);
  }
  const std::vector<LiveFrame> live = snapshotLive(*flash);
  CHECK(live.size() == 5);
  for (size_t i = 0; i < live.size(); ++i) {
    CHECK(live[i].seq == i + 1);
    CHECK(sameFrame(live[i], frameFor(static_cast<uint32_t>(i))));
    CHECK(live[i].recovered && !live[i].frozen);
  }
}

void testQueueFull() {
  g_test = "queue_full_16";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  for (uint32_t n = 0; n < 16; ++n) CHECK(log->append(frameFor(n), nullptr) == Status::Ok);
  const int programs = flash->programs;
  const std::vector<uint8_t> before = flash->data;
  CHECK(log->append(frameFor(99), nullptr) == Status::Full);
  CHECK(flash->programs == programs && flash->data == before);  // nothing written
  const std::vector<LiveFrame> live = snapshotLive(*flash);
  CHECK(live.size() == 16);
  for (size_t i = 0; i < live.size(); ++i) CHECK(sameFrame(live[i], frameFor(static_cast<uint32_t>(i))));
}

void testFreezeAckPersistence() {
  g_test = "freeze_ack_persistence";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  for (uint32_t n = 0; n < 3; ++n) log->append(frameFor(n), nullptr);
  CHECK(log->freeze(1, false) == Status::Ok);
  CHECK(log->acknowledge(2) == Status::InvalidArgument);  // FIFO head only
  CHECK(log->acknowledge(1) == Status::Ok);
  CHECK(log->freeze(2, true) == Status::Ok);
  CHECK(log->freeze(2, false) == Status::InvalidArgument);  // decision is final
  CHECK(findRecord(*flash, 2, 2, 0) > 0 && findRecord(*flash, 2, 2, 1) > 0);  // 2 copies

  std::unique_ptr<FrameLog> reboot(new FrameLog());
  CHECK(reboot->open(flash.get(), 1) == OpenResult::Recovered);
  LiveFrame head;
  CHECK(reboot->peekHead(&head) && head.seq == 2);
  CHECK(head.frozen && head.buffered && head.freeze_copies == 2);
  CHECK(reboot->liveCount() == 2);
  // Recovered never-frozen frame: maintain() freezes it with buffered=true.
  for (int i = 0; i < 6; ++i) reboot->maintain();
  CHECK(reboot->acknowledge(2) == Status::Ok);
  CHECK(reboot->peekHead(&head) && head.seq == 3 && head.frozen && head.buffered &&
        head.freeze_copies == 2);
}

void testSingleFreezeCopyToppedUp() {
  g_test = "single_freeze_copy";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  log->append(frameFor(0), nullptr);
  log->append(frameFor(1), nullptr);
  CHECK(log->freeze(1, false) == Status::Ok);
  const long copy1 = findRecord(*flash, 2, 1, 1);
  CHECK(copy1 > 0);
  flash->data[copy1 + 17] ^= 0x01;  // decay of the second copy (valid record follows? no)
  std::unique_ptr<FrameLog> reboot(new FrameLog());
  CHECK(reboot->open(flash.get(), 1) == OpenResult::Recovered);
  LiveFrame head;
  CHECK(reboot->peekHead(&head) && head.frozen && !head.buffered && head.freeze_copies == 1);
  CHECK(!head.freeze_unknown);
  // The damaged tail is closed: a prepared sector is needed first (mqttTask
  // always runs maintain() before freezing).
  CHECK(reboot->freeze(1, false) == Status::NoSpace);
  for (int i = 0; i < 4; ++i) reboot->maintain();
  CHECK(reboot->freeze(1, false) == Status::Ok);  // top-up keeps the value
  CHECK(reboot->peekHead(&head) && head.freeze_copies == 2 && !head.buffered);
}

void testCrcCorruptionResync() {
  g_test = "crc_corruption_resync";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  for (uint32_t n = 0; n < 3; ++n) log->append(frameFor(n), nullptr);
  const long rec2 = findRecord(*flash, 1, 2);
  CHECK(rec2 > 0);
  flash->data[rec2 + 50] ^= 0x10;  // body byte of FRAME 2
  std::unique_ptr<FrameLog> reboot(new FrameLog());
  CHECK(reboot->open(flash.get(), 1) == OpenResult::Recovered);
  CHECK(reboot->counters().corrupt_records >= 1);
  const std::vector<LiveFrame> live = snapshotLive(*flash);
  CHECK(live.size() == 2);
  if (live.size() == 2) {
    CHECK(live[0].seq == 1 && live[1].seq == 3);  // FRAME 2 lost, FRAME 3 kept
    CHECK(sameFrame(live[1], frameFor(2)));
  }
  uint32_t seq = 0;
  // The corrupt sector is closed: the next append goes to a prepared sector.
  for (int i = 0; i < 4; ++i) reboot->maintain();
  CHECK(reboot->append(frameFor(10), &seq) == Status::Ok && seq == 4);
}

void testFreezeUnknownOnDecay() {
  g_test = "freeze_unknown_on_decay";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  log->append(frameFor(0), nullptr);
  CHECK(log->freeze(1, true) == Status::Ok);  // "published" with buffered=true
  log->append(frameFor(1), nullptr);          // valid record after both copies
  flash->data[findRecord(*flash, 2, 1, 0) + 16] ^= 0x01;
  flash->data[findRecord(*flash, 2, 1, 1) + 16] ^= 0x01;
  std::unique_ptr<FrameLog> reboot(new FrameLog());
  CHECK(reboot->open(flash.get(), 1) == OpenResult::Recovered);
  LiveFrame head;
  CHECK(reboot->peekHead(&head) && head.seq == 1 && head.freeze_unknown && !head.frozen);
  CHECK(reboot->freeze(1, true) == Status::Blocked);
  CHECK(reboot->counters().freeze_unknown == 1);
  for (int i = 0; i < 5; ++i) reboot->maintain();  // never guesses
  CHECK(reboot->peekHead(&head) && head.freeze_unknown && !head.frozen);
}

void testOverCapacity() {
  g_test = "over_capacity";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  for (uint32_t n = 0; n < 16; ++n) log->append(frameFor(n), nullptr);
  for (uint32_t s = 1; s <= 4; ++s) {
    CHECK(log->freeze(s, false) == Status::Ok);
    CHECK(log->acknowledge(s) == Status::Ok);
  }
  for (int i = 0; i < 4; ++i) log->maintain();  // sector 0 is nearly full
  for (uint32_t n = 16; n < 20; ++n) CHECK(log->append(frameFor(n), nullptr) == Status::Ok);
  for (uint32_t s = 1; s <= 4; ++s) flash->data[findRecord(*flash, 3, s) + 20] ^= 0x01;
  const std::vector<uint8_t> before = flash->data;
  std::unique_ptr<FrameLog> reboot(new FrameLog());
  CHECK(reboot->open(flash.get(), 1) == OpenResult::OverCapacity);  // 20 > 16
  CHECK(!reboot->ready() && flash->data == before);
}

void testWriteFailures() {
  g_test = "write_failures";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  for (int i = 0; i < 4; ++i) log->maintain();  // prepared sector available
  CHECK(log->append(frameFor(0), nullptr) == Status::Ok);
  flash->fail_programs = 1;  // driver error before any byte
  uint32_t seq = 0;
  CHECK(log->append(frameFor(1), &seq) == Status::IoError);
  CHECK(log->liveCount() == 1);
  flash->fail_after_bytes = 100;  // partial write, then error
  for (int i = 0; i < 4; ++i) log->maintain();
  CHECK(log->append(frameFor(2), nullptr) == Status::IoError);
  CHECK(log->liveCount() == 1);
  for (int i = 0; i < 4; ++i) log->maintain();
  flash->corrupt_programs = 1;  // read-back differs
  CHECK(log->append(frameFor(3), nullptr) == Status::VerifyFailed);
  CHECK(log->liveCount() == 1);
  for (int i = 0; i < 4; ++i) log->maintain();
  CHECK(log->append(frameFor(4), &seq) == Status::Ok);
  CHECK(seq == 5);  // failed attempts consumed 2..4: never reused
  CHECK(log->counters().write_failures >= 2 && log->counters().verify_failures >= 1);
  const std::vector<LiveFrame> live = snapshotLive(*flash);
  CHECK(live.size() == 2);  // rejected frames never resurface after a reboot
  if (live.size() == 2) CHECK(live[0].seq == 1 && live[1].seq == 5);
}

void testNoSpaceWithoutPreparedSector() {
  g_test = "no_space_without_prepared";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  // Fill sector 0 without maintenance (frames confirmed at once, so the
  // capacity is never the limit): appends stop at the sector end.
  int ok = 0;
  Status last = Status::Ok;
  for (uint32_t n = 0; n < 40 && last == Status::Ok; ++n) {
    uint32_t seq = 0;
    last = log->append(frameFor(n), &seq);
    if (last != Status::Ok) break;
    ++ok;
    // Control records need room too: near the end they may hit NoSpace.
    last = log->freeze(seq, false);
    if (last == Status::Ok) last = log->acknowledge(seq);
  }
  CHECK(ok > 0 && ok < 40);
  CHECK(last == Status::NoSpace);
  CHECK(!log->hasPreparedSector());
  CHECK(log->maintain() && log->hasPreparedSector());  // PREPARE fits in the reserve
  CHECK(log->append(frameFor(50), nullptr) == Status::Ok);
}

void testReadFailureOnOpen() {
  g_test = "read_failure_on_open";
  auto flash = freshInitialized(6);
  flash->fail_reads = 1;
  const std::vector<uint8_t> before = flash->data;
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(flash.get(), 1) == OpenResult::IoError);
  CHECK(!log->ready() && flash->data == before);
}

void testReclaimAndSeqNoReuse() {
  g_test = "reclaim_seq_no_reuse";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  uint32_t last_seq = 0;
  for (uint32_t n = 0; n < 200; ++n) {
    for (int i = 0; i < 3; ++i) log->maintain();
    uint32_t seq = 0;
    CHECK(log->append(frameFor(n), &seq) == Status::Ok);
    CHECK(seq > last_seq);
    last_seq = seq;
    CHECK(log->freeze(seq, false) == Status::Ok);
    CHECK(log->acknowledge(seq) == Status::Ok);
  }
  for (int i = 0; i < 6; ++i) log->maintain();
  CHECK(log->counters().reclaimed_sectors >= 5);  // the ring rotated
  CHECK(flash->erases >= 5);
  std::unique_ptr<FrameLog> reboot(new FrameLog());
  CHECK(reboot->open(flash.get(), 1) == OpenResult::Recovered);
  CHECK(reboot->liveCount() == 0);
  for (int i = 0; i < 4; ++i) reboot->maintain();
  uint32_t seq = 0;
  CHECK(reboot->append(frameFor(500), &seq) == Status::Ok);
  CHECK(seq > last_seq);  // floor + max seen: no reuse after reclaim
}

void testEraseFailureQuarantine() {
  g_test = "erase_failure_quarantine";
  auto flash = freshInitialized(6);
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  flash->fail_erases = 3;
  for (uint32_t n = 0; n < 80; ++n) {
    for (int i = 0; i < 3; ++i) log->maintain();
    uint32_t seq = 0;
    CHECK(log->append(frameFor(n), &seq) == Status::Ok);
    log->freeze(seq, true);
    log->acknowledge(seq);
  }
  CHECK(log->counters().erase_failures == 3);
  CHECK(log->counters().quarantined_sectors == 1);
  CHECK(log->liveCount() == 0);  // store kept working on the other sectors
}

void testDirtySectorQuarantinedNotErased() {
  g_test = "dirty_sector_quarantined";
  auto flash = freshInitialized(6);
  // Unknown bytes in a sector the store never used (no intent names it).
  for (int i = 0; i < 64; ++i) flash->data[4 * kSectorSize + i] = static_cast<uint8_t>(i);
  const std::vector<uint8_t> foreign(flash->data.begin() + 4 * kSectorSize,
                                     flash->data.begin() + 5 * kSectorSize);
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(flash.get(), 1) == OpenResult::Recovered);
  CHECK(log->counters().quarantined_sectors == 1);
  for (uint32_t n = 0; n < 120; ++n) {
    for (int i = 0; i < 3; ++i) log->maintain();
    uint32_t seq = 0;
    if (log->append(frameFor(n), &seq) == Status::Ok) {
      log->freeze(seq, false);
      log->acknowledge(seq);
    }
  }
  const std::vector<uint8_t> after(flash->data.begin() + 4 * kSectorSize,
                                   flash->data.begin() + 5 * kSectorSize);
  CHECK(after == foreign);  // never erased nor written
}

void testPowerCutExhaustive() {
  g_test = "power_cut_exhaustive";
  auto base = freshInitialized(6);
  // Reference run to measure how many bytes the workload programs.
  long total = 0;
  {
    SimFlash flash = *base;
    std::unique_ptr<FrameLog> log(new FrameLog());
    log->open(&flash, 1);
    Model model;
    const long before = flash.programmed;
    runWorkload(*log, flash, model, 260, 42);
    total = flash.programmed - before;
    std::unique_ptr<FrameLog> reboot;
    verifyRecovered(flash, model, reboot);
    CHECK(model.acked.size() > 10 && flash.erases > 0);  // reclaim exercised
  }
  int runs = 0;
  for (long cut = 0; cut <= total; ++cut) {
    SimFlash flash = *base;
    std::unique_ptr<FrameLog> log(new FrameLog());
    log->open(&flash, 1);
    flash.program_budget = cut;
    Model model;
    runWorkload(*log, flash, model, 260, 42);
    std::unique_ptr<FrameLog> reboot;
    if (!verifyRecovered(flash, model, reboot)) {
      printf("  power cut after %ld bytes: recovery failed\n", cut);
      continue;
    }
    for (int i = 0; i < 8; ++i) reboot->maintain();
    uint32_t seq = 0;
    if (reboot->append(frameFor(9999), &seq) == Status::Ok) CHECK(seq > model.max_seq);
    ++runs;
  }
  printf("  power_cut_exhaustive: %d cut points over %ld programmed bytes\n", runs, total);
  CHECK(runs == total + 1);
}

void testPowerCutDuringErase() {
  g_test = "power_cut_during_erase";
  const long cuts[] = {0, 1, 16, 2048, 4095};
  for (long cut : cuts) {
    auto flash = freshInitialized(6);
    std::unique_ptr<FrameLog> log(new FrameLog());
    log->open(flash.get(), 1);
    Model model;
    // Run until the first erase happens, with the erase interrupted.
    flash->erase_cut_bytes = cut;
    runWorkload(*log, *flash, model, 2000, 7);
    CHECK(flash->dead);
    std::unique_ptr<FrameLog> reboot;
    if (!verifyRecovered(*flash, model, reboot)) continue;
    for (int i = 0; i < 10; ++i) reboot->maintain();  // completes the erase
    CHECK(reboot->counters().quarantined_sectors == 0);
    Model cont = model;
    runWorkload(*reboot, *flash, cont, 300, 11);
    std::unique_ptr<FrameLog> again;
    verifyRecovered(*flash, cont, again);
  }
}

void repeatedReboots(uint32_t sectors) {
  auto flash = freshInitialized(sectors);
  Model model;
  std::unique_ptr<FrameLog> log(new FrameLog());
  log->open(flash.get(), 1);
  uint32_t r = 2024;
  for (int round = 0; round < 300; ++round) {
    r = r * 1664525u + 1013904223u;
    flash->program_budget = static_cast<long>(r % 3000);
    if ((r >> 20) % 7 == 0) flash->erase_cut_bytes = static_cast<long>((r >> 8) % 4096);
    runWorkload(*log, *flash, model, 120, r);
    if (!verifyRecovered(*flash, model, log)) {
      printf("  reboot round %d: recovery failed\n", round);
      return;
    }
  }
  const gateway_frame_log::Counters& c = log->counters();
  printf("  %u sectors: acked=%u live=%u quarantined=%u unlogged_prepares=%u closed=%u "
         "corrupt=%u reclaimed=%u\n",
         sectors, static_cast<unsigned>(model.acked.size()),
         static_cast<unsigned>(log->liveCount()), c.quarantined_sectors, c.unlogged_prepares,
         c.closed_sectors, c.corrupt_records, c.reclaimed_sectors);
  CHECK(model.acked.size() > 100);
  CHECK(log->counters().quarantined_sectors == 0);
}

// Small ring (6 sectors): every cut lands inside a write, so torn tails close
// many sectors while old ones still hold live frames.
void testRepeatedReboots() {
  g_test = "repeated_reboots";
  repeatedReboots(6);
}

// Production geometry (352 sectors of the "spiffs" partition).
void testRepeatedReboots352() {
  g_test = "repeated_reboots_352";
  repeatedReboots(352);
}

void testInitializationInterrupted() {
  g_test = "initialization_interrupted";
  SimFlash flash(6);
  flash.program_budget = 10;  // power cut while writing the first header
  std::unique_ptr<FrameLog> log(new FrameLog());
  CHECK(log->open(&flash, 7) == OpenResult::IoError);
  flash.powerOn();
  std::unique_ptr<FrameLog> reboot(new FrameLog());
  const std::vector<uint8_t> before = flash.data;
  // Partial structure: never formatted automatically (owner policy 3.1).
  CHECK(reboot->open(&flash, 7) == OpenResult::ForeignData);
  CHECK(flash.data == before);
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"init_virgin", testInitVirgin},
      {"foreign_data_rejected", testForeignDataRejected},
      {"incompatible_version", testIncompatibleVersion},
      {"bad_geometry", testBadGeometry},
      {"frame_roundtrip_fifo", testFrameRoundTripAndFifo},
      {"queue_full_16", testQueueFull},
      {"freeze_ack_persistence", testFreezeAckPersistence},
      {"single_freeze_copy", testSingleFreezeCopyToppedUp},
      {"crc_corruption_resync", testCrcCorruptionResync},
      {"freeze_unknown_on_decay", testFreezeUnknownOnDecay},
      {"over_capacity", testOverCapacity},
      {"write_failures", testWriteFailures},
      {"no_space_without_prepared", testNoSpaceWithoutPreparedSector},
      {"read_failure_on_open", testReadFailureOnOpen},
      {"reclaim_seq_no_reuse", testReclaimAndSeqNoReuse},
      {"erase_failure_quarantine", testEraseFailureQuarantine},
      {"dirty_sector_quarantined", testDirtySectorQuarantinedNotErased},
      {"initialization_interrupted", testInitializationInterrupted},
      {"power_cut_exhaustive", testPowerCutExhaustive},
      {"power_cut_during_erase", testPowerCutDuringErase},
      {"repeated_reboots", testRepeatedReboots},
      {"repeated_reboots_352", testRepeatedReboots352},
  };
  int failed_tests = 0;
  for (const auto& t : tests) {
    const int before = g_failures;
    t.fn();
    const bool pass = g_failures == before;
    if (!pass) ++failed_tests;
    printf("%s %s\n", pass ? "PASS" : "FAIL", t.name);
  }
  const int count = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
  printf("\n%d tests, %d passed, %d failed; %d checks, %d check failures\n", count,
         count - failed_tests, failed_tests, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
