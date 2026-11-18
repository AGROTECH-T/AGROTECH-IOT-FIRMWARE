#include "CoordinatorCounters.h"

CoordinatorCounters::CoordinatorCounters()
    : persisted_seq_(0), ready_(false) {
  state_.boot_counter = 0;
  state_.seq = 0;
  state_.boot_exhausted = true;
  state_.seq_exhausted = true;
}

bool CoordinatorCounters::begin() {
  ready_ = false;
  if (!preferences_.begin("agrotech-co", false)) {
    return false;
  }

  const uint32_t stored_boot = preferences_.getUInt("boot", 0);
  agrotech::CounterState planned;
  const agrotech::Status status =
      agrotech::planNewBoot(stored_boot, &planned);
  if (status != agrotech::Status::Ok) {
    state_ = planned;
    return false;
  }

  if (!writeAndVerify("boot", planned.boot_counter)) {
    return false;
  }
  if (!writeAndVerify("seq", planned.seq)) {
    return false;
  }

  state_ = planned;
  persisted_seq_ = planned.seq;
  ready_ = true;
  return true;
}

bool CoordinatorCounters::takeSeq(uint32_t* out_seq) {
  if (!ready_) {
    return false;
  }
  return agrotech::takeSequence(&state_, out_seq) == agrotech::Status::Ok;
}

bool CoordinatorCounters::checkpoint() {
  if (!ready_) {
    return false;
  }
  if (state_.seq == persisted_seq_) {
    return true;
  }
  if (!writeAndVerify("seq", state_.seq)) {
    return false;
  }
  persisted_seq_ = state_.seq;
  return true;
}

uint32_t CoordinatorCounters::bootCounter() const {
  return state_.boot_counter;
}

uint32_t CoordinatorCounters::currentSeq() const {
  return state_.seq;
}

bool CoordinatorCounters::seqExhausted() const {
  return state_.seq_exhausted;
}

bool CoordinatorCounters::bootExhausted() const {
  return state_.boot_exhausted;
}

bool CoordinatorCounters::ready() const {
  return ready_;
}

bool CoordinatorCounters::writeAndVerify(const char* key, uint32_t value) {
  if (preferences_.putUInt(key, value) != sizeof(uint32_t)) {
    return false;
  }
  if (!preferences_.isKey(key)) {
    return false;
  }
  return preferences_.getUInt(key, 0) == value;
}
