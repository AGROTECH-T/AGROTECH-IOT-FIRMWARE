#include "GatewayFrameLog.h"

#include <string.h>

namespace gateway_frame_log {
namespace {

// Sector header (32 bytes, little-endian):
//   0 magic "AGQ1" | 4 format_version u16 | 6 header_size u16 (32)
//   8 sector_seq u32 | 12 region_uid u32 | 16 region_sectors u32
//  20 frame_seq_floor u32 | 24 reserved u32 (0) | 28 crc32 of bytes 0..27
constexpr uint32_t kSectorMagic = 0x31514741u;  // 'A' 'G' 'Q' '1'

// Record header (16 bytes, little-endian), then body, zero padding, crc32:
//   0 'A' | 1 'R' | 2 type | 3 record_version | 4 length u16 | 6 reserved u16
//   8 seq u32 | 12 region_uid u32 | ... | length-4 crc32 of bytes 0..length-5
constexpr uint8_t kRecordMagic0 = 0x41;  // 'A'
constexpr uint8_t kRecordMagic1 = 0x52;  // 'R'

enum RecordType : uint8_t {
  kTypeFrame = 1,
  kTypeFreeze = 2,
  kTypeAck = 3,
  kTypeCancel = 4,
  kTypeInit = 5,
  kTypePrepare = 6,
  kTypeReclaim = 7,
};

// FRAME body offsets.
constexpr uint32_t kFrameLengthAt = 16;
constexpr uint32_t kFrameRssiAt = 18;
constexpr uint32_t kFrameSnrAt = 20;
constexpr uint32_t kFrameRxTimeAt = 24;
constexpr uint32_t kFrameFlagsAt = 32;
constexpr uint32_t kFrameBytesAt = 36;
// Control body offsets: a, b, c (c is reserved, always 0).
constexpr uint32_t kControlAAt = 16;
constexpr uint32_t kControlBAt = 20;
constexpr uint32_t kControlCAt = 24;

static_assert(sizeof(float) == 4, "snr is persisted as IEEE-754 binary32 bits");
static_assert(kFrameBytesAt + kMaxFrameBytes <= kFrameRecordSize - 4,
              "FRAME body overlaps its CRC");
static_assert(kMaxSectors * kSectorSize <= 0xFFFFFFFFu, "region offsets are 32-bit");

uint32_t crc32(const uint8_t* data, size_t length) {
  // CRC-32/ISO-HDLC (reflected 0xEDB88320), bitwise: small and dependency-free.
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t get32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

bool allErased(const uint8_t* p, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (p[i] != 0xFF) return false;
  }
  return true;
}

uint32_t recordLength(uint8_t type) {
  switch (type) {
    case kTypeFrame:
      return kFrameRecordSize;
    case kTypeFreeze:
    case kTypeAck:
    case kTypeCancel:
    case kTypeInit:
    case kTypePrepare:
    case kTypeReclaim:
      return kControlRecordSize;
    default:
      return 0;
  }
}

void encodeHeader(uint8_t* record, uint8_t type, uint32_t length, uint32_t seq,
                  uint32_t region_uid) {
  memset(record, 0, length);
  record[0] = kRecordMagic0;
  record[1] = kRecordMagic1;
  record[2] = type;
  record[3] = kRecordVersion;
  put16(record + 4, static_cast<uint16_t>(length));
  put16(record + 6, 0);
  put32(record + 8, seq);
  put32(record + 12, region_uid);
}

void sealRecord(uint8_t* record, uint32_t length) {
  put32(record + length - 4, crc32(record, length - 4));
}

}  // namespace

const char* statusText(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::Unavailable: return "store unavailable";
    case Status::Full: return "queue full";
    case Status::NoSpace: return "no prepared sector";
    case Status::IoError: return "flash I/O error";
    case Status::VerifyFailed: return "flash verify failed";
    case Status::NotFound: return "frame not found";
    case Status::InvalidArgument: return "invalid argument";
    case Status::Blocked: return "frame blocked (FREEZE unknown)";
  }
  return "unknown";
}

const char* openResultText(OpenResult result) {
  switch (result) {
    case OpenResult::Recovered: return "recovered";
    case OpenResult::Initialized: return "initialized (erased region)";
    case OpenResult::BadRegion: return "bad region geometry";
    case OpenResult::ForeignData: return "unknown data in region (not erased)";
    case OpenResult::Incompatible: return "incompatible or inconsistent store";
    case OpenResult::OverCapacity: return "more live frames than capacity";
    case OpenResult::IoError: return "flash I/O error";
  }
  return "unknown";
}

FrameLog::FrameLog() { reset(); }

void FrameLog::reset() {
  flash_ = nullptr;
  memset(sectors_, 0, sizeof(sectors_));
  memset(live_, 0, sizeof(live_));
  live_count_ = 0;
  sector_count_ = 0;
  region_uid_ = 0;
  next_seq_ = 1;
  next_sector_seq_ = 1;
  tail_ = -1;
  prepared_ = -1;
  tail_offset_ = 0;
  tail_closed_ = true;
  ready_ = false;
  memset(&counters_, 0, sizeof(counters_));
  replay_pos_ = 0;
  max_frame_seq_ = 0;
  max_seq_seen_ = 0;
  max_intent_sector_seq_ = 0;
  has_nonterminal_corruption_ = false;
  nonterminal_corruption_pos_ = 0;
  replay_overflow_ = false;
}

// ---- open / recovery -------------------------------------------------------

OpenResult FrameLog::open(FlashRegion* flash, uint32_t new_region_uid) {
  reset();
  if (flash == nullptr) return OpenResult::BadRegion;
  flash_ = flash;
  const uint32_t size = flash_->size();
  if (size == 0 || size % kSectorSize != 0) return OpenResult::BadRegion;
  const uint32_t count = size / kSectorSize;
  if (count < kMinSectors || count > kMaxSectors) return OpenResult::BadRegion;
  sector_count_ = count;

  // Pass 1 (read-only): classify every sector by its header.
  uint32_t valid = 0;
  uint32_t dirty = 0;
  for (uint32_t s = 0; s < count; ++s) {
    uint8_t h[kSectorHeaderSize];
    if (!flash_->read(s * kSectorSize, h, sizeof(h))) return OpenResult::IoError;
    SectorInfo& info = sectors_[s];
    if (allErased(h, sizeof(h))) {
      info.state = kHeaderBlank;
      continue;
    }
    if (get32(h) != kSectorMagic || get16(h + 6) != kSectorHeaderSize ||
        get32(h + 28) != crc32(h, 28)) {
      info.state = kDirty;
      ++dirty;
      continue;
    }
    // A checksummed AGQ1 header that this firmware does not fully understand
    // is never reinterpreted, reused or erased.
    if (get16(h + 4) != kFormatVersion || get32(h + 16) != count ||
        get32(h + 8) == 0 || get32(h + 24) != 0) {
      return OpenResult::Incompatible;
    }
    const uint32_t uid = get32(h + 12);
    if (valid == 0) {
      region_uid_ = uid;
    } else if (uid != region_uid_) {
      return OpenResult::Incompatible;
    }
    info.state = kValid;
    info.seq = get32(h + 8);
    info.floor = get32(h + 20);
    ++valid;
  }

  if (valid == 0) {
    // First use: initialize only a region whose every byte is erased.
    if (dirty > 0) return OpenResult::ForeignData;
    for (uint32_t s = 0; s < count; ++s) {
      bool erased = false;
      if (!rangeErased(s, 0, &erased)) return OpenResult::IoError;
      if (!erased) return OpenResult::ForeignData;
    }
    return initialize(new_region_uid);
  }

  // Order valid sectors by their sequence number; duplicates are inconsistent.
  uint16_t order[kMaxSectors];
  uint32_t n = 0;
  for (uint32_t s = 0; s < count; ++s) {
    if (sectors_[s].state != kValid) continue;
    uint32_t i = n++;
    while (i > 0 && sectors_[order[i - 1]].seq > sectors_[s].seq) {
      order[i] = order[i - 1];
      --i;
    }
    order[i] = static_cast<uint16_t>(s);
  }
  for (uint32_t i = 1; i < n; ++i) {
    if (sectors_[order[i]].seq == sectors_[order[i - 1]].seq) return OpenResult::Incompatible;
  }

  // Pass 2 (read-only): replay records in log order.
  for (uint32_t i = 0; i < n; ++i) {
    bool io_ok = true;
    if (!replaySector(order[i], &io_ok) || !io_ok) return OpenResult::IoError;
  }
  if (replay_overflow_) return OpenResult::OverCapacity;

  // Intents decide which non-valid sectors are provably ours.
  for (uint32_t s = 0; s < count; ++s) {
    SectorInfo& info = sectors_[s];
    if (info.state == kValid && info.intent == kIntentReclaim && info.intent_seq == info.seq) {
      info.state = kPendingErase;  // RECLAIM logged, erase not completed
    } else if (info.state == kDirty) {
      if (info.intent != kIntentNone) {
        info.state = kPendingErase;  // interrupted erase or header write
      } else {
        info.state = kQuarantined;   // provenance unknown: never touched
        ++counters_.quarantined_sectors;
      }
    }
  }

  const uint32_t newest = order[n - 1];
  if (sectors_[newest].state != kValid) return OpenResult::Incompatible;
  tail_ = static_cast<int32_t>(newest);
  tail_offset_ = sectors_[newest].end;
  tail_closed_ = sectors_[newest].closed != 0;

  if (live_count_ > kLiveCapacity) return OpenResult::OverCapacity;
  for (size_t i = 0; i < live_count_; ++i) {
    LiveFrame& f = live_[i];
    ++sectors_[f.sector].live;
    // A FREEZE copy may hide in a corrupt area that lies after this FRAME
    // and before later valid records: its `buffered` value is unknown.
    if (!f.frozen && has_nonterminal_corruption_ &&
        nonterminal_corruption_pos_ >= f.record_pos) {
      f.freeze_unknown = true;
    }
    if (f.freeze_unknown) ++counters_.freeze_unknown;
  }

  uint32_t next = max_seq_seen_ + 1;
  uint32_t max_sector_seq = max_intent_sector_seq_;
  for (uint32_t s = 0; s < count; ++s) {
    const SectorInfo& info = sectors_[s];
    if (info.state != kValid && info.state != kPendingErase) continue;
    if (info.floor > next) next = info.floor;
    if (info.seq > max_sector_seq) max_sector_seq = info.seq;
  }
  next_seq_ = next == 0 ? 1 : next;
  next_sector_seq_ = max_sector_seq + 1;
  ready_ = true;
  return OpenResult::Recovered;
}

OpenResult FrameLog::initialize(uint32_t region_uid) {
  if (region_uid == 0 || region_uid == 0xFFFFFFFFu) region_uid = 0x31514741u;
  region_uid_ = region_uid;
  for (uint32_t s = 0; s < sector_count_; ++s) sectors_[s].state = kBlank;
  next_seq_ = 1;
  next_sector_seq_ = 1;
  if (!writeSectorHeader(0, next_sector_seq_, next_seq_)) return OpenResult::IoError;
  SectorInfo& first = sectors_[0];
  first.state = kValid;
  first.seq = next_sector_seq_;
  first.floor = next_seq_;
  first.end = kSectorHeaderSize;
  ++next_sector_seq_;
  tail_ = 0;
  tail_offset_ = kSectorHeaderSize;
  tail_closed_ = false;
  prepared_ = -1;
  // Explicit initialization mark, verified like every other record.
  if (appendControl(kTypeInit, 0, kFormatVersion, sector_count_, false) != Status::Ok) {
    return OpenResult::IoError;
  }
  ready_ = true;
  return OpenResult::Initialized;
}

bool FrameLog::replaySector(uint32_t sector, bool* io_ok) {
  *io_ok = true;
  SectorInfo& info = sectors_[sector];
  uint32_t offset = kSectorHeaderSize;
  bool clean = false;
  bool stopped = false;
  uint8_t head[kWriteAlign];
  uint8_t record[kFrameRecordSize];
  while (offset + kWriteAlign <= kSectorSize) {
    if (!flash_->read(sector * kSectorSize + offset, head, kWriteAlign)) {
      *io_ok = false;
      return false;
    }
    bool parsed = false;
    uint32_t length = 0;
    if (allErased(head, kWriteAlign)) {
      bool erased = false;
      if (!rangeErased(sector, offset, &erased)) {
        *io_ok = false;
        return false;
      }
      if (erased) {
        clean = true;  // end of this sector's records
        break;
      }
      // Programmed bytes after an erased gap: append-only writes never
      // produce that, so the gap is treated as corruption.
    } else {
      bool read_ok = true;
      parsed = readValidRecord(sector, offset, head, record, &length, &read_ok);
      if (!read_ok) {
        *io_ok = false;
        return false;
      }
    }
    if (parsed) {
      applyRecord(sector, record);
      offset += length;
      continue;
    }
    noteCorruption(sector);
    const uint32_t corruption_pos = replay_pos_;
    uint32_t next = 0;
    if (!resync(sector, offset + kWriteAlign, &next, io_ok)) return false;
    if (next == 0) {
      // Nothing valid after it in this sector: the signature of a write that
      // was interrupted (the sector is closed right after a failed write and
      // after a reboot). Such a record was never verified, so nothing was
      // published relying on it.
      stopped = true;
      break;
    }
    // A valid record follows inside the same sector: the damaged area was
    // already written and verified (decay or interrupted erase), so it may
    // have held a FREEZE copy that a publish relied on.
    has_nonterminal_corruption_ = true;
    if (corruption_pos > nonterminal_corruption_pos_) nonterminal_corruption_pos_ = corruption_pos;
    offset = next;
  }
  if (!clean && !stopped && offset + kWriteAlign > kSectorSize) {
    clean = info.closed == 0;  // sector exactly filled by valid records
  }
  info.end = static_cast<uint16_t>(offset > kSectorSize ? kSectorSize : offset);
  if (!clean && info.closed == 0) {
    info.closed = 1;
    ++counters_.closed_sectors;
  }
  return true;
}

bool FrameLog::readValidRecord(uint32_t sector, uint32_t offset, const uint8_t* head,
                               uint8_t* record, uint32_t* length, bool* io_ok) {
  *io_ok = true;
  if (head[0] != kRecordMagic0 || head[1] != kRecordMagic1 || head[3] != kRecordVersion) {
    return false;
  }
  const uint8_t type = head[2];
  const uint32_t len = recordLength(type);
  if (len == 0 || get16(head + 4) != len || get16(head + 6) != 0) return false;
  if (offset + len > kSectorSize) return false;
  if (get32(head + 12) != region_uid_) return false;
  if (!flash_->read(sector * kSectorSize + offset, record, len)) {
    *io_ok = false;
    return false;
  }
  if (memcmp(record, head, kWriteAlign) != 0) return false;
  if (get32(record + len - 4) != crc32(record, len - 4)) return false;

  // Strict semantic checks: a record is accepted only if every field is one
  // this firmware could have written.
  const uint32_t seq = get32(record + 8);
  switch (type) {
    case kTypeFrame: {
      if (seq == 0 || seq <= max_frame_seq_) return false;  // strictly increasing
      const uint16_t frame_length = get16(record + kFrameLengthAt);
      if (frame_length == 0 || frame_length > kMaxFrameBytes) return false;
      if (record[kFrameFlagsAt] > 1 || record[kFrameFlagsAt + 1] != 0 ||
          record[kFrameFlagsAt + 2] != 0 || record[kFrameFlagsAt + 3] != 0) {
        return false;
      }
      for (uint32_t i = kFrameBytesAt + frame_length; i < len - 4; ++i) {
        if (record[i] != 0) return false;
      }
      break;
    }
    case kTypeFreeze:
      if (seq == 0 || get32(record + kControlAAt) > 1 || get32(record + kControlBAt) > 1 ||
          get32(record + kControlCAt) != 0) {
        return false;
      }
      break;
    case kTypeAck:
    case kTypeCancel:
      if (seq == 0 || get32(record + kControlAAt) != 0 || get32(record + kControlBAt) != 0 ||
          get32(record + kControlCAt) != 0) {
        return false;
      }
      break;
    case kTypeInit:
      if (seq != 0 || get32(record + kControlAAt) != kFormatVersion ||
          get32(record + kControlBAt) != sector_count_ || get32(record + kControlCAt) != 0) {
        return false;
      }
      break;
    case kTypePrepare:
    case kTypeReclaim:
      if (seq != 0 || get32(record + kControlAAt) >= sector_count_ ||
          get32(record + kControlBAt) == 0 || get32(record + kControlCAt) != 0) {
        return false;
      }
      break;
    default:
      return false;
  }
  *length = len;
  return true;
}

// After a corrupt area, accept the next record only if it passes the full
// validation above (magic, version, fixed length, region id, CRC32, field
// ranges and, for FRAME, a strictly increasing sequence). If none is found
// the rest of the sector is abandoned (sector closed).
bool FrameLog::resync(uint32_t sector, uint32_t from, uint32_t* next, bool* io_ok) {
  *next = 0;
  *io_ok = true;
  uint8_t head[kWriteAlign];
  uint8_t record[kFrameRecordSize];
  for (uint32_t o = from; o + kControlRecordSize <= kSectorSize; o += kWriteAlign) {
    if (!flash_->read(sector * kSectorSize + o, head, kWriteAlign)) {
      *io_ok = false;
      return false;
    }
    if (allErased(head, kWriteAlign)) continue;
    uint32_t length = 0;
    bool read_ok = true;
    if (readValidRecord(sector, o, head, record, &length, &read_ok)) {
      *next = o;
      return true;
    }
    if (!read_ok) {
      *io_ok = false;
      return false;
    }
  }
  return true;
}

void FrameLog::noteCorruption(uint32_t sector) {
  ++counters_.corrupt_records;
  SectorInfo& info = sectors_[sector];
  if (info.closed == 0) {
    info.closed = 1;
    ++counters_.closed_sectors;
  }
}

void FrameLog::applyRecord(uint32_t sector, const uint8_t* record) {
  ++replay_pos_;
  const uint8_t type = record[2];
  const uint32_t seq = get32(record + 8);
  if (seq > max_seq_seen_) max_seq_seen_ = seq;

  switch (type) {
    case kTypeFrame: {
      max_frame_seq_ = seq;
      if (live_count_ >= kReplayCapacity) {
        replay_overflow_ = true;
        return;
      }
      LiveFrame& f = live_[live_count_++];
      memset(&f, 0, sizeof(f));
      f.seq = seq;
      f.length = get16(record + kFrameLengthAt);
      f.rssi = static_cast<int16_t>(get16(record + kFrameRssiAt));
      const uint32_t snr_bits = get32(record + kFrameSnrAt);
      memcpy(&f.snr, &snr_bits, sizeof(f.snr));
      f.rx_time = static_cast<int64_t>(get64(record + kFrameRxTimeAt));
      f.time_synced_at_rx = record[kFrameFlagsAt] != 0;
      memcpy(f.bytes, record + kFrameBytesAt, f.length);
      f.recovered = true;
      f.sector = static_cast<uint16_t>(sector);
      f.record_pos = replay_pos_;
      return;
    }
    case kTypeFreeze: {
      const int index = findLive(seq);
      if (index < 0) return;
      LiveFrame& f = live_[index];
      const bool buffered = get32(record + kControlAAt) != 0;
      if (!f.frozen) {
        f.frozen = true;
        f.buffered = buffered;
        f.freeze_copies = 1;
      } else if (f.buffered == buffered) {
        if (f.freeze_copies < 2) ++f.freeze_copies;
      } else {
        f.freeze_unknown = true;  // two valid decisions disagree: never guess
        ++counters_.freeze_conflicts;
      }
      return;
    }
    case kTypeAck:
    case kTypeCancel: {
      const int index = findLive(seq);
      if (index >= 0) removeLive(static_cast<size_t>(index));
      return;
    }
    case kTypePrepare:
    case kTypeReclaim: {
      const uint32_t target = get32(record + kControlAAt);
      const uint32_t target_seq = get32(record + kControlBAt);
      if (target_seq > max_intent_sector_seq_) max_intent_sector_seq_ = target_seq;
      SectorInfo& t = sectors_[target];
      t.intent = type == kTypePrepare ? kIntentPrepare : kIntentReclaim;
      t.intent_seq = target_seq;
      if (type == kTypeReclaim && t.state == kValid && t.seq == target_seq) {
        // Logged only once every frame of that sector was confirmed.
        for (size_t i = live_count_; i-- > 0;) {
          if (live_[i].sector == target) removeLive(i);
        }
      }
      return;
    }
    default:
      return;  // INIT: validated, no state
  }
}

bool FrameLog::rangeErased(uint32_t sector, uint32_t from, bool* erased) {
  *erased = false;
  uint8_t chunk[128];
  for (uint32_t o = from; o < kSectorSize;) {
    const uint32_t n = (kSectorSize - o) < sizeof(chunk) ? (kSectorSize - o) : sizeof(chunk);
    if (!flash_->read(sector * kSectorSize + o, chunk, n)) return false;
    if (!allErased(chunk, n)) return true;
    o += n;
  }
  *erased = true;
  return true;
}

// ---- writes ----------------------------------------------------------------

bool FrameLog::writeSectorHeader(uint32_t sector, uint32_t seq, uint32_t floor) {
  uint8_t h[kSectorHeaderSize];
  memset(h, 0, sizeof(h));
  put32(h, kSectorMagic);
  put16(h + 4, kFormatVersion);
  put16(h + 6, kSectorHeaderSize);
  put32(h + 8, seq);
  put32(h + 12, region_uid_);
  put32(h + 16, sector_count_);
  put32(h + 20, floor);
  put32(h + 24, 0);
  put32(h + 28, crc32(h, 28));
  const uint32_t at = sector * kSectorSize;
  if (!flash_->program(at, h, sizeof(h))) {
    ++counters_.write_failures;
    return false;
  }
  uint8_t check[kSectorHeaderSize];
  if (!flash_->read(at, check, sizeof(check)) || memcmp(check, h, sizeof(h)) != 0) {
    ++counters_.verify_failures;
    return false;
  }
  return true;
}

// Appends into the tail sector. Ordinary records always leave room for one
// control record, so a PREPARE intent can still be logged in a full tail.
Status FrameLog::appendRecord(const uint8_t* record, uint32_t length, bool may_use_reserve,
                              uint32_t* sector_out) {
  if (tail_ < 0) return Status::NoSpace;
  const uint32_t reserve = may_use_reserve ? 0 : kControlRecordSize;
  if (tail_closed_ || tail_offset_ + length + reserve > kSectorSize) {
    if (prepared_ < 0) return Status::NoSpace;
    tail_ = prepared_;
    prepared_ = -1;
    tail_offset_ = kSectorHeaderSize;
    tail_closed_ = false;
  }
  SectorInfo& info = sectors_[tail_];
  const uint32_t at = static_cast<uint32_t>(tail_) * kSectorSize + tail_offset_;
  if (!flash_->program(at, record, length)) {
    // The area may be partially programmed: never append after it.
    ++counters_.write_failures;
    tail_closed_ = true;
    if (info.closed == 0) {
      info.closed = 1;
      ++counters_.closed_sectors;
    }
    return Status::IoError;
  }
  uint8_t check[kFrameRecordSize];
  if (!flash_->read(at, check, length) || memcmp(check, record, length) != 0) {
    ++counters_.verify_failures;
    tail_closed_ = true;
    if (info.closed == 0) {
      info.closed = 1;
      ++counters_.closed_sectors;
    }
    return Status::VerifyFailed;
  }
  tail_offset_ += length;
  info.end = static_cast<uint16_t>(tail_offset_);
  if (sector_out != nullptr) *sector_out = static_cast<uint32_t>(tail_);
  return Status::Ok;
}

Status FrameLog::appendControl(uint8_t type, uint32_t seq, uint32_t a, uint32_t b,
                               bool may_use_reserve) {
  uint8_t record[kControlRecordSize];
  encodeHeader(record, type, kControlRecordSize, seq, region_uid_);
  put32(record + kControlAAt, a);
  put32(record + kControlBAt, b);
  put32(record + kControlCAt, 0);
  sealRecord(record, kControlRecordSize);
  return appendRecord(record, kControlRecordSize, may_use_reserve, nullptr);
}

Status FrameLog::append(const LiveFrame& frame, uint32_t* out_seq) {
  if (!ready_) return Status::Unavailable;
  if (live_count_ >= kLiveCapacity) return Status::Full;
  if (frame.length == 0 || frame.length > kMaxFrameBytes) return Status::InvalidArgument;
  if (next_seq_ == 0 || next_seq_ == 0xFFFFFFFFu) return Status::Unavailable;
  // Consumed even if the write fails: a sequence number is never reused.
  const uint32_t seq = next_seq_++;

  uint8_t record[kFrameRecordSize];
  encodeHeader(record, kTypeFrame, kFrameRecordSize, seq, region_uid_);
  put16(record + kFrameLengthAt, frame.length);
  put16(record + kFrameRssiAt, static_cast<uint16_t>(frame.rssi));
  uint32_t snr_bits = 0;
  memcpy(&snr_bits, &frame.snr, sizeof(snr_bits));
  put32(record + kFrameSnrAt, snr_bits);
  put64(record + kFrameRxTimeAt, static_cast<uint64_t>(frame.rx_time));
  record[kFrameFlagsAt] = frame.time_synced_at_rx ? 1 : 0;
  memcpy(record + kFrameBytesAt, frame.bytes, frame.length);
  sealRecord(record, kFrameRecordSize);

  uint32_t sector = 0;
  const Status status = appendRecord(record, kFrameRecordSize, false, &sector);
  if (status != Status::Ok) {
    if (status == Status::IoError || status == Status::VerifyFailed) {
      // The FRAME might still be readable later: cancel it explicitly so a
      // frame that was rejected now never resurfaces after a reboot.
      if (appendControl(kTypeCancel, seq, 0, 0, false) != Status::Ok) {
        ++counters_.cancel_failures;
      }
    }
    return status;
  }

  LiveFrame& f = live_[live_count_++];
  f = frame;
  f.seq = seq;
  f.frozen = false;
  f.buffered = false;
  f.freeze_copies = 0;
  f.freeze_unknown = false;
  f.recovered = false;
  f.sector = static_cast<uint16_t>(sector);
  f.record_pos = 0;
  ++sectors_[sector].live;
  if (out_seq != nullptr) *out_seq = seq;
  return Status::Ok;
}

bool FrameLog::peekHead(LiveFrame* out) const {
  if (!ready_ || live_count_ == 0 || out == nullptr) return false;
  *out = live_[0];
  return true;
}

Status FrameLog::freeze(uint32_t seq, bool buffered) {
  if (!ready_) return Status::Unavailable;
  const int index = findLive(seq);
  if (index < 0) return Status::NotFound;
  LiveFrame& f = live_[index];
  if (f.freeze_unknown) return Status::Blocked;
  if (f.frozen && f.buffered != buffered) return Status::InvalidArgument;
  while (f.freeze_copies < 2) {
    const Status status =
        appendControl(kTypeFreeze, seq, buffered ? 1 : 0, f.freeze_copies, false);
    if (status != Status::Ok) return status;
    f.frozen = true;
    f.buffered = buffered;
    ++f.freeze_copies;
  }
  return Status::Ok;
}

Status FrameLog::acknowledge(uint32_t seq) {
  if (!ready_) return Status::Unavailable;
  if (live_count_ == 0 || live_[0].seq != seq) return Status::InvalidArgument;  // FIFO head only
  const Status status = appendControl(kTypeAck, seq, 0, 0, false);
  if (status != Status::Ok) return status;  // frame stays live
  SectorInfo& info = sectors_[live_[0].sector];
  if (info.live > 0) --info.live;
  removeLive(0);
  return Status::Ok;
}

int FrameLog::findLive(uint32_t seq) const {
  for (size_t i = 0; i < live_count_; ++i) {
    if (live_[i].seq == seq) return static_cast<int>(i);
  }
  return -1;
}

void FrameLog::removeLive(size_t index) {
  if (index >= live_count_) return;
  for (size_t i = index + 1; i < live_count_; ++i) live_[i - 1] = live_[i];
  --live_count_;
  memset(&live_[live_count_], 0, sizeof(LiveFrame));
}

// ---- maintenance (MQTT task only) -----------------------------------------

bool FrameLog::eraseAndVerify(uint32_t sector) {
  SectorInfo& info = sectors_[sector];
  bool erased = false;
  if (flash_->eraseSector(sector) && rangeErased(sector, 0, &erased) && erased) {
    memset(&info, 0, sizeof(info));
    info.state = kBlank;
    return true;
  }
  ++counters_.erase_failures;
  if (++info.erase_attempts >= kMaxEraseAttempts) {
    info.state = kQuarantined;
    ++counters_.quarantined_sectors;
  }
  return false;
}

bool FrameLog::prepareNext() {
  if (tail_ < 0 || next_sector_seq_ == 0xFFFFFFFFu) return false;
  for (uint32_t k = 1; k < sector_count_; ++k) {
    const uint32_t s = (static_cast<uint32_t>(tail_) + k) % sector_count_;
    SectorInfo& info = sectors_[s];
    if (info.state != kBlank && info.state != kHeaderBlank) continue;
    if (info.state == kHeaderBlank) {
      bool erased = false;
      if (!rangeErased(s, 0, &erased)) return false;
      if (!erased) {
        // Programmed bytes under an erased header and no intent naming it.
        if (info.intent != kIntentNone) {
          info.state = kPendingErase;
        } else {
          info.state = kQuarantined;
          ++counters_.quarantined_sectors;
        }
        return true;
      }
      info.state = kBlank;
    }
    const uint32_t seq = next_sector_seq_++;
    // Intent first: a torn header write then leaves a sector provably ours.
    if (appendControl(kTypePrepare, 0, s, seq, true) == Status::Ok) {
      info.intent = kIntentPrepare;
      info.intent_seq = seq;
    } else {
      ++counters_.unlogged_prepares;
    }
    if (!writeSectorHeader(s, seq, next_seq_)) {
      if (info.intent != kIntentNone) {
        info.state = kPendingErase;
      } else {
        info.state = kQuarantined;
        ++counters_.quarantined_sectors;
      }
      return true;
    }
    memset(&info, 0, sizeof(info));
    info.state = kValid;
    info.seq = seq;
    info.floor = next_seq_;
    info.end = kSectorHeaderSize;
    prepared_ = static_cast<int32_t>(s);
    ++counters_.prepared_sectors;
    return true;
  }
  return false;  // no erased sector left in the ring
}

bool FrameLog::reclaimOldest() {
  int32_t oldest = -1;
  for (uint32_t s = 0; s < sector_count_; ++s) {
    const SectorInfo& info = sectors_[s];
    if (info.state != kValid) continue;
    if (static_cast<int32_t>(s) == tail_ || static_cast<int32_t>(s) == prepared_) continue;
    if (oldest < 0 || info.seq < sectors_[oldest].seq) oldest = static_cast<int32_t>(s);
  }
  if (oldest < 0) return false;
  SectorInfo& info = sectors_[oldest];
  if (info.live != 0) return false;  // FIFO: nothing older can be freed either
  const uint32_t seq = info.seq;
  if (appendControl(kTypeReclaim, 0, static_cast<uint32_t>(oldest), seq, false) != Status::Ok) {
    return false;
  }
  info.state = kPendingErase;
  info.intent = kIntentReclaim;
  info.intent_seq = seq;
  if (eraseAndVerify(static_cast<uint32_t>(oldest))) ++counters_.reclaimed_sectors;
  return true;
}

bool FrameLog::maintain() {
  if (!ready_) return false;
  // 1) Finish erases whose provenance is proven by a durable intent. Runs
  //    before any reclaim, so intents are never erased while still needed.
  for (uint32_t s = 0; s < sector_count_; ++s) {
    if (sectors_[s].state == kPendingErase) {
      if (eraseAndVerify(s)) ++counters_.reclaimed_sectors;
      return true;
    }
  }
  // 2) Keep one prepared sector ahead of the tail for loop()'s appends.
  if (prepared_ < 0 && prepareNext()) return true;
  // 3) Recovered frames never frozen get buffered=true (they waited across a
  //    reboot); recovered frozen frames keep their value and get 2 copies.
  for (size_t i = 0; i < live_count_; ++i) {
    LiveFrame& f = live_[i];
    if (!f.recovered || f.freeze_unknown) continue;
    if (f.frozen && f.freeze_copies >= 2) continue;
    freeze(f.seq, f.frozen ? f.buffered : true);
    return true;
  }
  // 4) Free the oldest sector once all its frames are confirmed.
  return reclaimOldest();
}

}  // namespace gateway_frame_log
