#include "GatewayFrameStore.h"

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <stdlib.h>
#include <string.h>

#include "GatewayConfig.h"

namespace gateway_frame_store {
namespace {

using gateway_frame_log::FlashRegion;
using gateway_frame_log::FrameLog;
using gateway_frame_log::LiveFrame;
using gateway_frame_log::OpenResult;
using gateway_frame_log::Status;
using gateway_frame_log::kSectorSize;

// Existing data partition, accessed through the ESP-IDF partition API. Only
// erase-aligned sector erases and writes into erased bytes are issued.
class PartitionRegion : public FlashRegion {
 public:
  void attach(const esp_partition_t* partition) { partition_ = partition; }
  uint32_t size() const override {
    return partition_ == nullptr ? 0 : static_cast<uint32_t>(partition_->size);
  }
  bool read(uint32_t offset, uint8_t* out, uint32_t length) override {
    return partition_ != nullptr && esp_partition_read(partition_, offset, out, length) == ESP_OK;
  }
  bool program(uint32_t offset, const uint8_t* data, uint32_t length) override {
    return partition_ != nullptr && esp_partition_write(partition_, offset, data, length) == ESP_OK;
  }
  bool eraseSector(uint32_t sector) override {
    return partition_ != nullptr &&
           esp_partition_erase_range(partition_, sector * kSectorSize, kSectorSize) == ESP_OK;
  }

 private:
  const esp_partition_t* partition_ = nullptr;
};

// Diagnostic builds only: NOR-like RAM region (program ANDs bits, erase sets
// 0xFF), so the same log code runs without ever touching the flash.
class RamRegion : public FlashRegion {
 public:
  bool allocate(uint32_t size) {
    data_ = static_cast<uint8_t*>(malloc(size));
    if (data_ == nullptr) return false;
    memset(data_, 0xFF, size);
    size_ = size;
    return true;
  }
  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, uint8_t* out, uint32_t length) override {
    if (!inRange(offset, length)) return false;
    memcpy(out, data_ + offset, length);
    return true;
  }
  bool program(uint32_t offset, const uint8_t* data, uint32_t length) override {
    if (!inRange(offset, length)) return false;
    for (uint32_t i = 0; i < length; ++i) data_[offset + i] &= data[i];
    return true;
  }
  bool eraseSector(uint32_t sector) override {
    const uint32_t offset = sector * kSectorSize;
    if (!inRange(offset, kSectorSize)) return false;
    memset(data_ + offset, 0xFF, kSectorSize);
    return true;
  }

 private:
  bool inRange(uint32_t offset, uint32_t length) const {
    return data_ != nullptr && offset <= size_ && length <= size_ - offset;
  }
  uint8_t* data_ = nullptr;
  uint32_t size_ = 0;
};

constexpr uint32_t kDiagnosticSectors = gateway_frame_log::kMinSectors;

FrameLog g_log;
PartitionRegion g_partition;
RamRegion g_ram;
SemaphoreHandle_t g_lock = nullptr;
Mode g_mode = Mode::Disabled;
bool g_ready = false;
const char* g_state = "not started";
Counters g_counters = {};

class Lock {
 public:
  Lock() {
    locked_ = g_lock != nullptr &&
              xSemaphoreTake(g_lock, pdMS_TO_TICKS(GATEWAY_FRAME_STORE_LOCK_TIMEOUT_MS)) == pdTRUE;
    if (!locked_) ++g_counters.lock_timeouts;
  }
  ~Lock() {
    if (locked_) xSemaphoreGive(g_lock);
  }
  bool held() const { return locked_; }

 private:
  bool locked_ = false;
};

bool fail(const char* reason) {
  g_ready = false;
  g_state = reason;
  Serial.printf("[STORE] UNAVAILABLE: %s; LoRa frames will be rejected, nothing is published\n",
                reason);
  return false;
}

uint32_t newRegionUid() {
  uint32_t uid = esp_random();
  if (uid == 0 || uid == 0xFFFFFFFFu) uid ^= 0x5A5AA5A5u;
  return uid;
}

}  // namespace

bool begin(bool diagnostic_build) {
  if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
  if (g_lock == nullptr) return fail("mutex allocation failed");

  OpenResult result;
  if (diagnostic_build) {
    // Explicit, never silent: the diagnostic build does not use the flash.
    g_mode = Mode::Volatile;
    if (!g_ram.allocate(kDiagnosticSectors * kSectorSize)) {
      return fail("diagnostic RAM store allocation failed");
    }
    Serial.println("[STORE] DIAGNOSTIC build: volatile RAM store; flash partition not accessed");
    result = g_log.open(&g_ram, newRegionUid());
  } else {
    g_mode = Mode::Durable;
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                 GATEWAY_FRAME_STORE_PARTITION_LABEL);
    if (partition == nullptr) {
      return fail("data/spiffs partition '" GATEWAY_FRAME_STORE_PARTITION_LABEL "' not found");
    }
    if (partition->size != GATEWAY_FRAME_STORE_PARTITION_SIZE) {
      return fail("partition size differs from GATEWAY_FRAME_STORE_PARTITION_SIZE");
    }
    if (partition->erase_size != kSectorSize) return fail("partition erase size is not 4096");
    // Erased bytes of an encrypted partition do not read back as 0xFF, which
    // the first-use and torn-write checks rely on.
    if (partition->encrypted) return fail("encrypted partition not supported");
    if (partition->readonly) return fail("partition is read-only");
    g_partition.attach(partition);
    result = g_log.open(&g_partition, newRegionUid());
  }

  if (result != OpenResult::Recovered && result != OpenResult::Initialized) {
    // Never erased or rewritten here: an operator decides what to do.
    return fail(gateway_frame_log::openResultText(result));
  }
  g_ready = true;
  g_state = result == OpenResult::Initialized ? "ready (initialized now)" : "ready (recovered)";
  Serial.printf("[STORE] %s mode=%s sectors=%lu live=%u capacity=%u\n", g_state,
                g_mode == Mode::Durable ? "durable" : "volatile-diagnostic",
                static_cast<unsigned long>(g_log.sectorCount()),
                static_cast<unsigned>(g_log.liveCount()),
                static_cast<unsigned>(gateway_frame_log::kLiveCapacity));
  const gateway_frame_log::Counters& c = g_log.counters();
  if (c.corrupt_records || c.quarantined_sectors || c.freeze_unknown || c.freeze_conflicts) {
    Serial.printf("[STORE] recovery anomalies: corrupt=%lu quarantined=%lu freeze_unknown=%lu "
                  "freeze_conflicts=%lu\n",
                  static_cast<unsigned long>(c.corrupt_records),
                  static_cast<unsigned long>(c.quarantined_sectors),
                  static_cast<unsigned long>(c.freeze_unknown),
                  static_cast<unsigned long>(c.freeze_conflicts));
  }
  return true;
}

bool ready() { return g_ready; }
Mode mode() { return g_mode; }
const char* stateText() { return g_state; }

Status store(const uint8_t* frame, size_t length, int64_t rx_time, int rssi, float snr,
             bool time_synced_at_rx, bool link_was_down, uint32_t received_ms) {
  if (!g_ready) {
    ++g_counters.rejected_unavailable;
    return Status::Unavailable;
  }
  if (frame == nullptr || length == 0 || length > gateway_frame_log::kMaxFrameBytes) {
    ++g_counters.rejected_write;
    return Status::InvalidArgument;
  }
  LiveFrame item;
  memset(&item, 0, sizeof(item));
  item.length = static_cast<uint16_t>(length);
  memcpy(item.bytes, frame, length);
  item.rssi = static_cast<int16_t>(rssi);
  item.snr = snr;
  item.rx_time = rx_time;
  item.time_synced_at_rx = time_synced_at_rx;
  item.buffered_hint = link_was_down;
  item.enqueued_ms = received_ms;

  Lock lock;
  if (!lock.held()) {
    ++g_counters.rejected_unavailable;
    return Status::Unavailable;
  }
  const Status status = g_log.append(item, nullptr);
  switch (status) {
    case Status::Ok: ++g_counters.accepted; break;
    case Status::Full: ++g_counters.rejected_full; break;
    case Status::Unavailable: ++g_counters.rejected_unavailable; break;
    default: ++g_counters.rejected_write; break;
  }
  return status;
}

bool peekHead(LiveFrame* out) {
  if (!g_ready) return false;
  Lock lock;
  return lock.held() && g_log.peekHead(out);
}

Status freeze(uint32_t seq, bool buffered) {
  if (!g_ready) return Status::Unavailable;
  Lock lock;
  if (!lock.held()) return Status::Unavailable;
  const Status status = g_log.freeze(seq, buffered);
  if (status != Status::Ok) ++g_counters.freeze_failures;
  return status;
}

Status acknowledge(uint32_t seq) {
  if (!g_ready) return Status::Unavailable;
  Lock lock;
  if (!lock.held()) return Status::Unavailable;
  const Status status = g_log.acknowledge(seq);
  if (status != Status::Ok) ++g_counters.ack_failures;
  return status;
}

bool maintain() {
  if (!g_ready) return false;
  Lock lock;
  return lock.held() && g_log.maintain();
}

size_t liveCount() {
  if (!g_ready) return 0;
  Lock lock;
  return lock.held() ? g_log.liveCount() : 0;
}

const Counters& counters() { return g_counters; }

void printStatus(const char* prefix) {
  size_t live = 0;
  bool prepared = false;
  gateway_frame_log::Counters c = {};
  {
    Lock lock;
    if (lock.held()) {
      live = g_log.liveCount();
      prepared = g_log.hasPreparedSector();
      c = g_log.counters();
    }
  }
  Serial.printf("[%s] store state=%s mode=%s live=%u/%u prepared=%s\n", prefix, g_state,
                g_mode == Mode::Durable    ? "durable"
                : g_mode == Mode::Volatile ? "volatile-diagnostic"
                                           : "disabled",
                static_cast<unsigned>(live),
                static_cast<unsigned>(gateway_frame_log::kLiveCapacity), prepared ? "yes" : "no");
  Serial.printf("[%s] store accepted=%lu rejected full=%lu unavailable=%lu write=%lu "
                "lock_timeouts=%lu freeze_fail=%lu ack_fail=%lu\n",
                prefix, static_cast<unsigned long>(g_counters.accepted),
                static_cast<unsigned long>(g_counters.rejected_full),
                static_cast<unsigned long>(g_counters.rejected_unavailable),
                static_cast<unsigned long>(g_counters.rejected_write),
                static_cast<unsigned long>(g_counters.lock_timeouts),
                static_cast<unsigned long>(g_counters.freeze_failures),
                static_cast<unsigned long>(g_counters.ack_failures));
  Serial.printf("[%s] flash corrupt=%lu closed=%lu quarantined=%lu write_fail=%lu "
                "verify_fail=%lu erase_fail=%lu reclaimed=%lu prepared=%lu unlogged_prep=%lu "
                "cancel_fail=%lu freeze_conflict=%lu freeze_unknown=%lu\n",
                prefix, static_cast<unsigned long>(c.corrupt_records),
                static_cast<unsigned long>(c.closed_sectors),
                static_cast<unsigned long>(c.quarantined_sectors),
                static_cast<unsigned long>(c.write_failures),
                static_cast<unsigned long>(c.verify_failures),
                static_cast<unsigned long>(c.erase_failures),
                static_cast<unsigned long>(c.reclaimed_sectors),
                static_cast<unsigned long>(c.prepared_sectors),
                static_cast<unsigned long>(c.unlogged_prepares),
                static_cast<unsigned long>(c.cancel_failures),
                static_cast<unsigned long>(c.freeze_conflicts),
                static_cast<unsigned long>(c.freeze_unknown));
}

}  // namespace gateway_frame_store
