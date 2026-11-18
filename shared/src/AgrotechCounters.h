#ifndef AGROTECH_COUNTERS_H
#define AGROTECH_COUNTERS_H

#include <stdint.h>

#include "AgrotechProtocol.h"

namespace agrotech {

struct CounterState {
  uint32_t boot_counter;
  uint32_t seq;
  bool boot_exhausted;
  bool seq_exhausted;
};

Status planNewBoot(uint32_t stored_boot_counter, CounterState* out_state);
Status takeSequence(CounterState* state, uint32_t* out_seq);

}  // namespace agrotech

#endif
