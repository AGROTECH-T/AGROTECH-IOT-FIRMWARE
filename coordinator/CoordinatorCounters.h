#ifndef AGROTECH_COORDINATOR_COUNTERS_H
#define AGROTECH_COORDINATOR_COUNTERS_H

#include <Preferences.h>
#include <stdint.h>

#include "AgrotechCounters.h"

class CoordinatorCounters {
 public:
  CoordinatorCounters();

  bool begin();
  bool takeSeq(uint32_t* out_seq);
  bool checkpoint();

  uint32_t bootCounter() const;
  uint32_t currentSeq() const;
  bool seqExhausted() const;
  bool bootExhausted() const;
  bool ready() const;

 private:
  bool writeAndVerify(const char* key, uint32_t value);

  Preferences preferences_;
  agrotech::CounterState state_;
  uint32_t persisted_seq_;
  bool ready_;
};

#endif
