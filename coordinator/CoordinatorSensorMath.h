#ifndef AGROTECH_COORDINATOR_SENSOR_MATH_H
#define AGROTECH_COORDINATOR_SENSOR_MATH_H

#include <math.h>
#include <stdint.h>

#include "AgrotechProtocol.h"

namespace coordinator_sensor {

enum class Source : uint8_t { None, Bme680, Dht11 };

inline void clearReadings(agrotech::FrameFields* fields) {
  if (fields != nullptr) fields->reading_count = 0;
}

inline bool scaleX100(float value, int32_t minimum_x100,
                      int32_t maximum_x100, int32_t* out) {
  if (out == nullptr || !isfinite(value)) return false;
  const float scaled = value * 100.0f;
  if (!isfinite(scaled) || scaled < minimum_x100 || scaled > maximum_x100)
    return false;
  *out = static_cast<int32_t>(lroundf(scaled));
  return *out >= minimum_x100 && *out <= maximum_x100;
}

inline Source selectSource(bool bme_valid, bool dht_valid) {
  if (bme_valid) return Source::Bme680;
  if (dht_valid) return Source::Dht11;
  return Source::None;
}

// ---------------------------------------------------------------------------
// BUILD-IOT-SENSOR-FAILOVER-04: hardware-independent failover state machine.
// Pure C++11 (no Arduino), so it can be exercised later by host tests.
//
// Counters only ever count *consecutive* events:
//   - fail: consecutive BME680 failures, reset by any valid BME680 reading;
//   - ok:   consecutive valid BME680 readings while degraded, reset by any
//           BME680 failure;
//   - degraded: set when fail reaches F, cleared when ok reaches R.
// Both counters saturate, so old failures can never accumulate.
// The DHT11 result never changes a counter: it can only change what is
// published in the current cycle, never the BME680 recovery progress.
// ---------------------------------------------------------------------------

enum class FailoverState : uint8_t {
  Principal,
  Sospecha,
  Respaldo,
  Recuperacion,
  SinSensor
};

struct FailoverCounters {
  uint8_t fail;
  uint8_t ok;
  bool degraded;
};

struct FailoverDecision {
  FailoverState state;
  Source publish;           // None: no frame and no sequence consumed
  bool entered_degraded;    // degraded became true in this cycle
  bool recovered;           // degraded became false in this cycle
};

// D-1: the initial state is PRINCIPAL with zeroed counters. It describes the
// logic state only, not real sensor availability; a BME680 that failed to
// initialize at boot simply counts as a failure in the first cycle.
inline void initFailoverCounters(FailoverCounters* counters) {
  if (counters == nullptr) return;
  counters->fail = 0;
  counters->ok = 0;
  counters->degraded = false;
}

// Whether the DHT11 must be read in this cycle (after the BME680 result is
// known): when the BME680 failed, or while degraded (to keep publishing it
// during RECUPERACION).
inline bool failoverNeedsDht(const FailoverCounters& counters, bool bme_valid) {
  return !bme_valid || counters.degraded;
}

inline FailoverDecision failoverStep(FailoverCounters* counters, bool bme_valid,
                                     bool dht_valid, uint8_t fail_threshold,
                                     uint8_t recovery_threshold) {
  FailoverDecision decision;
  decision.state = FailoverState::SinSensor;
  decision.publish = Source::None;
  decision.entered_degraded = false;
  decision.recovered = false;
  if (counters == nullptr || fail_threshold == 0 || recovery_threshold == 0) {
    return decision;
  }

  if (bme_valid) {
    counters->fail = 0;  // D-2: a valid reading ends any suspicion
    if (counters->degraded) {
      if (counters->ok < recovery_threshold) ++counters->ok;
      if (counters->ok >= recovery_threshold) {
        counters->degraded = false;  // D-4: switch in this same cycle
        counters->ok = 0;
        decision.recovered = true;
      }
    } else {
      counters->ok = 0;
    }
  } else {
    counters->ok = 0;  // a failure during RECUPERACION restarts recovery
    if (counters->fail < fail_threshold) ++counters->fail;
    if (counters->fail >= fail_threshold && !counters->degraded) {
      counters->degraded = true;
      decision.entered_degraded = true;
    }
  }

  if (bme_valid && !counters->degraded) {
    decision.state = FailoverState::Principal;
    decision.publish = Source::Bme680;
  } else if (bme_valid) {
    // RECUPERACION: keep the DHT11 while available; D-3: if the DHT11 fails,
    // publish the (valid) BME680 on its own channels and keep counting.
    decision.state = FailoverState::Recuperacion;
    decision.publish = dht_valid ? Source::Dht11 : Source::Bme680;
  } else if (!dht_valid) {
    decision.state = FailoverState::SinSensor;
    decision.publish = Source::None;
  } else if (counters->degraded) {
    decision.state = FailoverState::Respaldo;
    decision.publish = Source::Dht11;
  } else {
    decision.state = FailoverState::Sospecha;
    decision.publish = Source::Dht11;
  }
  return decision;
}

// K reinitialization timer. `wait` is the number of acquisition cycles to
// skip before the next attempt. Only consulted while the BME680 is not ready.
struct ReinitTimer {
  uint8_t wait;
};

// Called at the start of a cycle while the BME680 is not ready. Returns true
// when an attempt must be made in this cycle; after an attempt the next one
// happens exactly `interval` cycles later.
inline bool reinitDue(ReinitTimer* timer, uint8_t interval) {
  if (timer == nullptr || interval == 0) return false;
  if (timer->wait == 0) {
    timer->wait = static_cast<uint8_t>(interval - 1);
    return true;
  }
  --timer->wait;
  return false;
}

// The setup attempt counts as cycle 0: the next attempt is at cycle `interval`.
inline void reinitAfterAttempt(ReinitTimer* timer, uint8_t interval) {
  if (timer != nullptr && interval > 0) {
    timer->wait = static_cast<uint8_t>(interval - 1);
  }
}

// Attempt in the very next cycle (sensor just declared unavailable).
inline void reinitNextCycle(ReinitTimer* timer) {
  if (timer != nullptr) timer->wait = 0;
}

inline const char* failoverStateName(FailoverState state) {
  switch (state) {
    case FailoverState::Principal:
      return "PRINCIPAL";
    case FailoverState::Sospecha:
      return "SOSPECHA";
    case FailoverState::Respaldo:
      return "RESPALDO";
    case FailoverState::Recuperacion:
      return "RECUPERACION";
    case FailoverState::SinSensor:
      return "SIN_SENSOR";
  }
  return "UNKNOWN";
}

// Reason for a [SOURCE] transition (state or published source changed).
inline const char* failoverTransitionReason(FailoverState previous,
                                            Source previous_publish,
                                            const FailoverDecision& decision) {
  if (decision.state == FailoverState::SinSensor) return "both_failed";
  if (previous == FailoverState::SinSensor) return "source_restored";
  if (decision.recovered) return "bme_recovered";
  if (decision.entered_degraded) return "bme_degraded";
  switch (decision.state) {
    case FailoverState::Principal:
      return "bme_restored";
    case FailoverState::Sospecha:
      return "bme_fail";
    case FailoverState::Respaldo:
      return "bme_fail";
    case FailoverState::Recuperacion:
      if (previous == FailoverState::Recuperacion &&
          previous_publish != decision.publish) {
        return decision.publish == Source::Bme680 ? "dht_failed"
                                                  : "dht_restored";
      }
      return "bme_recovering";
    case FailoverState::SinSensor:
      return "both_failed";
  }
  return "unknown";
}

}  // namespace coordinator_sensor

#endif
