#ifndef AGROTECH_GATEWAY_FRAME_LOG_H
#define AGROTECH_GATEWAY_FRAME_LOG_H

// ===========================================================================
// MQTT-QUEUE-PERSISTENCE-05 - durable append-only frame log (pure C++).
//
// No Arduino, FreeRTOS or ESP-IDF dependency: every flash access goes through
// FlashRegion, so the codec, the recovery and the power-cut behaviour can be
// exercised on a host with a simulated NOR flash. A FrameLog instance is NOT
// thread-safe; GatewayFrameStore owns the lock.
//
// Storage model (see docs/MQTT-QUEUE-PERSISTENCE-05.md):
//   * the region is a ring of 4096-byte sectors; each used sector starts with
//     a CRC-protected header carrying a never-reused sector sequence number;
//   * records are only ever appended into erased bytes, at 16-byte aligned
//     offsets and with 16-byte multiple lengths, and are never rewritten;
//   * every record is versioned, has a fixed length per type, carries the
//     region id and ends with a CRC32;
//   * FRAME holds one LoRa frame plus its reception metadata, FREEZE (written
//     twice) fixes the `buffered` decision before the first publish, ACK is
//     written after the matching PUBACK; PREPARE / RECLAIM record an intent
//     before a sector header write / a sector erase, so an interrupted
//     operation leaves a sector whose provenance is provable;
//   * a sector is erased only when it is the oldest one and holds no live
//     frame (FIFO), and only after its RECLAIM intent is durable.
// ===========================================================================

#include <stddef.h>
#include <stdint.h>

#include <AgrotechProtocol.h>

namespace gateway_frame_log {

constexpr uint32_t kSectorSize = 4096;
// Every offset and length is a multiple of 16 bytes, which also satisfies the
// encrypted-partition write rule of esp_partition_write().
constexpr uint32_t kWriteAlign = 16;
constexpr uint32_t kSectorHeaderSize = 32;
constexpr uint32_t kFrameRecordSize = 192;
constexpr uint32_t kControlRecordSize = 32;
// Live frames: stored and not yet confirmed by an ACK record, including the
// one being published or waiting for its PUBACK.
constexpr size_t kLiveCapacity = 16;
// Replay may transiently see more unconfirmed frames (only if ACK records
// were lost); recovery fails closed when the final count exceeds
// kLiveCapacity.
constexpr size_t kReplayCapacity = 24;
constexpr uint32_t kMinSectors = 4;
constexpr uint32_t kMaxSectors = 384;
constexpr uint16_t kFormatVersion = 1;
constexpr uint8_t kRecordVersion = 1;
constexpr size_t kMaxFrameBytes = agrotech::kMaxFrameBytes;
constexpr uint8_t kMaxEraseAttempts = 3;

static_assert(kFrameRecordSize % kWriteAlign == 0 &&
                  kControlRecordSize % kWriteAlign == 0 &&
                  kSectorHeaderSize % kWriteAlign == 0,
              "flash records must keep 16-byte alignment");
static_assert(36 + kMaxFrameBytes + 4 <= kFrameRecordSize,
              "FRAME record too small for the largest Frame v1");

// Raw NOR-flash region. Offsets are relative to the region start.
class FlashRegion {
 public:
  virtual ~FlashRegion() {}
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, uint8_t* out, uint32_t length) = 0;
  // Programs previously erased bytes (NOR semantics: bits only go 1 -> 0).
  virtual bool program(uint32_t offset, const uint8_t* data, uint32_t length) = 0;
  // Erases one kSectorSize sector back to 0xFF.
  virtual bool eraseSector(uint32_t sector) = 0;
};

// One stored frame. Only the "durable" fields are serialized (explicitly,
// field by field); the "this boot" fields live in RAM only.
struct LiveFrame {
  // Durable: FRAME record.
  uint32_t seq;
  uint16_t length;
  uint8_t bytes[kMaxFrameBytes];
  int16_t rssi;
  float snr;
  int64_t rx_time;
  bool time_synced_at_rx;
  // Durable: FREEZE records.
  bool frozen;
  bool buffered;
  uint8_t freeze_copies;   // verified FREEZE copies (publish needs 2)
  bool freeze_unknown;     // FREEZE may have been lost: never published
  // This boot only.
  bool recovered;          // rebuilt from flash during open()
  bool buffered_hint;      // link was down when the frame was received
  uint32_t enqueued_ms;
  uint16_t sector;         // sector holding the FRAME record
  uint32_t record_pos;     // replay ordinal of the FRAME record
};

enum class Status : uint8_t {
  Ok,
  Unavailable,      // store not open
  Full,             // kLiveCapacity live frames already stored
  NoSpace,          // no prepared sector to continue the log
  IoError,          // flash driver error
  VerifyFailed,     // read-back differs from what was programmed
  NotFound,
  InvalidArgument,
  Blocked,          // frame cannot be frozen safely (freeze_unknown)
};

enum class OpenResult : uint8_t {
  Recovered,        // existing store recovered (no flash write)
  Initialized,      // completely erased region initialized now
  BadRegion,        // geometry not acceptable
  ForeignData,      // programmed data that is not a valid store: untouched
  Incompatible,     // store of another version/geometry/id, or inconsistent
  OverCapacity,     // recovery found more than kLiveCapacity live frames
  IoError,
};

struct Counters {
  uint32_t corrupt_records;      // unparseable/checksum-failing areas
  uint32_t closed_sectors;       // sectors no longer appendable
  uint32_t quarantined_sectors;  // unknown provenance or failing erase
  uint32_t write_failures;
  uint32_t verify_failures;
  uint32_t erase_failures;
  uint32_t reclaimed_sectors;
  uint32_t prepared_sectors;
  uint32_t unlogged_prepares;    // header written without a PREPARE intent
  uint32_t cancel_failures;      // unaccepted FRAME left without CANCEL
  uint32_t freeze_conflicts;     // valid FREEZE copies disagree
  uint32_t freeze_unknown;       // recovered frames that cannot be frozen
};

const char* statusText(Status status);
const char* openResultText(OpenResult result);

class FrameLog {
 public:
  FrameLog();

  // Read-only recovery of an existing store, or initialization of a region
  // that is completely erased (0xFF). Anything else is never written.
  OpenResult open(FlashRegion* flash, uint32_t new_region_uid);

  bool ready() const { return ready_; }
  size_t liveCount() const { return live_count_; }
  uint32_t sectorCount() const { return sector_count_; }
  bool hasPreparedSector() const { return prepared_ >= 0; }
  const Counters& counters() const { return counters_; }

  // Appends and verifies a FRAME record; the frame is live only on Ok.
  Status append(const LiveFrame& frame, uint32_t* out_seq);
  // Copy of the oldest live frame (FIFO head).
  bool peekHead(LiveFrame* out) const;
  // Writes and verifies FREEZE copies until two exist for `seq`.
  Status freeze(uint32_t seq, bool buffered);
  // Head only: writes and verifies ACK, then drops the frame from RAM.
  Status acknowledge(uint32_t seq);
  // One bounded maintenance step (erase, prepare, recovered freeze, reclaim).
  // Returns true when it touched the flash. Never call it from loop().
  bool maintain();

 private:
  enum SectorState : uint8_t {
    kHeaderBlank = 0,  // header bytes erased; rest not verified yet
    kBlank,            // whole sector verified erased
    kValid,            // valid header of this store
    kDirty,            // programmed bytes without a valid header
    kPendingErase,     // ours (proven by an intent): erase is due
    kQuarantined,      // never touched again
  };
  enum Intent : uint8_t { kIntentNone = 0, kIntentPrepare, kIntentReclaim };

  struct SectorInfo {
    uint32_t seq;
    uint32_t floor;        // next frame seq when the sector was prepared
    uint32_t intent_seq;   // sector seq named by the latest intent
    uint16_t end;          // first unused offset (valid sectors)
    uint8_t state;
    uint8_t live;          // live FRAME records stored in this sector
    uint8_t intent;
    uint8_t closed;        // 1: corrupt/torn area, no further appends
    uint8_t erase_attempts;
  };

  void reset();
  OpenResult initialize(uint32_t region_uid);
  bool replaySector(uint32_t sector, bool* io_ok);
  bool readValidRecord(uint32_t sector, uint32_t offset, const uint8_t* head,
                       uint8_t* record, uint32_t* length, bool* io_ok);
  bool resync(uint32_t sector, uint32_t from, uint32_t* next, bool* io_ok);
  void noteCorruption(uint32_t sector);
  void applyRecord(uint32_t sector, const uint8_t* record);
  bool rangeErased(uint32_t sector, uint32_t from, bool* erased);
  bool writeSectorHeader(uint32_t sector, uint32_t seq, uint32_t floor);
  Status appendRecord(const uint8_t* record, uint32_t length, bool may_use_reserve,
                      uint32_t* sector_out);
  Status appendControl(uint8_t type, uint32_t seq, uint32_t a, uint32_t b,
                       bool may_use_reserve);
  int findLive(uint32_t seq) const;
  void removeLive(size_t index);
  bool eraseAndVerify(uint32_t sector);
  bool prepareNext();
  bool reclaimOldest();

  FlashRegion* flash_;
  SectorInfo sectors_[kMaxSectors];
  LiveFrame live_[kReplayCapacity];
  size_t live_count_;
  uint32_t sector_count_;
  uint32_t region_uid_;
  uint32_t next_seq_;
  uint32_t next_sector_seq_;
  int32_t tail_;
  int32_t prepared_;
  uint32_t tail_offset_;
  bool tail_closed_;
  bool ready_;
  Counters counters_;
  // Replay-only state.
  uint32_t replay_pos_;
  uint32_t max_frame_seq_;
  uint32_t max_seq_seen_;
  uint32_t max_intent_sector_seq_;
  bool has_nonterminal_corruption_;
  uint32_t nonterminal_corruption_pos_;
  bool replay_overflow_;
};

}  // namespace gateway_frame_log

#endif
