#include "AgrotechCounters.h"

namespace agrotech {

Status planNewBoot(uint32_t stored_boot_counter, CounterState* out_state) {
  if (out_state == nullptr) {
    return Status::InvalidArgument;
  }
  if (stored_boot_counter == 0xffffffffu) {
    out_state->boot_counter = stored_boot_counter;
    out_state->seq = 0;
    out_state->boot_exhausted = true;
    out_state->seq_exhausted = true;
    return Status::BootCounterExhausted;
  }

  out_state->boot_counter = stored_boot_counter + 1;
  out_state->seq = 0;
  out_state->boot_exhausted = false;
  out_state->seq_exhausted = false;
  return Status::Ok;
}

Status takeSequence(CounterState* state, uint32_t* out_seq) {
  if (state == nullptr || out_seq == nullptr) {
    return Status::InvalidArgument;
  }
  if (state->boot_exhausted) {
    return Status::BootCounterExhausted;
  }
  if (state->seq_exhausted) {
    return Status::SequenceExhausted;
  }

  *out_seq = state->seq;
  if (state->seq == 0xffffffffu) {
    state->seq_exhausted = true;
  } else {
    state->seq += 1;
  }
  return Status::Ok;
}

}  // namespace agrotech
