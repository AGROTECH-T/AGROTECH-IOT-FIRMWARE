// BUILD-IOT-SENSOR-FAILOVER-05: host tests of the REAL
// coordinator/CoordinatorSensors.cpp compiled against the test doubles in
// test_vectors/failover_mocks/ (Arduino, Wire, Adafruit_BME680, DHT).
// No hardware: the doubles model sensor presence/validity per cycle.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "Adafruit_BME680.h"
#include "Arduino.h"
#include "CoordinatorConfig.h"
#include "CoordinatorSensors.h"
#include "DHT.h"
#include "Wire.h"

uint32_t g_fake_millis = 0;
FakeSerial Serial;
TwoWire Wire;
FakeBmeState g_fake_bme;
FakeDhtState g_fake_dht;

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

using coordinator::SensorSource;
using coordinator_sensor::FailoverState;

struct Cycle {
  SensorSource source;
  FailoverState state;
  uint16_t channels[2];
  int32_t values[2];
  uint8_t reading_count;
  int begin_calls;  // BME680 begin() calls during this cycle
  std::string log;  // serial output of this cycle
};

void resetFakes() {
  g_fake_bme.present_address = 0x76;
  g_fake_bme.config_ok = true;
  g_fake_bme.perform_ok = true;
  g_fake_bme.next_temperature = 25.34f;
  g_fake_bme.next_humidity = 63.25f;
  g_fake_bme.begin_calls = 0;
  g_fake_bme.heater_off_calls = 0;
  g_fake_bme.heater_enabled = false;
  g_fake_dht.temperature = 30.10f;
  g_fake_dht.humidity = 70.60f;
  g_fake_dht.reads = 0;
  Wire.begin_result = true;
  Serial.out.clear();
}

void boot() {
  initializeSensors();
  Serial.out.clear();
}

Cycle runCycle() {
  g_fake_millis += COORDINATOR_TX_INTERVAL_MS;
  const int begin_before = g_fake_bme.begin_calls;
  Serial.out.clear();
  agrotech::FrameFields fields;
  agrotech::initFrameFields(&fields);
  fields.reading_count = 2;  // stale data from a previous cycle
  Cycle cycle;
  cycle.source = readConfiguredSensors(&fields);
  cycle.state = currentFailoverState();
  cycle.reading_count = fields.reading_count;
  cycle.channels[0] = fields.readings[0].channel;
  cycle.channels[1] = fields.readings[1].channel;
  cycle.values[0] = fields.readings[0].value;
  cycle.values[1] = fields.readings[1].value;
  cycle.begin_calls = g_fake_bme.begin_calls - begin_before;
  cycle.log = Serial.out;
  return cycle;
}

int countOf(const std::string& text, const char* needle) {
  int count = 0;
  size_t at = text.find(needle);
  while (at != std::string::npos) {
    ++count;
    at = text.find(needle, at + 1);
  }
  return count;
}

bool isBme(const Cycle& c) {
  return c.source == SensorSource::Bme680 && c.reading_count == 2 &&
         c.channels[0] == 1 && c.channels[1] == 2;
}

bool isDht(const Cycle& c) {
  return c.source == SensorSource::Dht11 && c.reading_count == 2 &&
         c.channels[0] == 3 && c.channels[1] == 4;
}

void testBootWithBme() {  // cases 1, 14, heater
  resetFakes();
  boot();
  CHECK(!g_fake_bme.heater_enabled && g_fake_bme.heater_off_calls == 1);
  const Cycle c1 = runCycle();
  CHECK(isBme(c1) && c1.state == FailoverState::Principal);
  CHECK(c1.values[0] == 2534 && c1.values[1] == 6325);
  CHECK(g_fake_dht.reads == 0);  // DHT11 not read in PRINCIPAL
  CHECK(countOf(c1.log, "[SOURCE]") == 0 && c1.begin_calls == 0);
}

void testBootWithoutBmeAndReconnect() {  // cases 2, 3, 4, 6, 7, 8, 12, 14, 17
  resetFakes();
  g_fake_bme.present_address = 0;
  boot();
  CHECK(g_fake_bme.begin_calls == 2);  // setup attempt (cycle 0): 0x77 and 0x76
  Cycle c[13];
  for (int i = 1; i <= 12; ++i) {
    if (i == 8) g_fake_bme.present_address = 0x76;  // reconnected before cycle 8
    c[i] = runCycle();
  }
  CHECK(isDht(c[1]) && c[1].state == FailoverState::Sospecha);
  CHECK(c[1].values[0] == 3010 && c[1].values[1] == 7060);
  CHECK(isDht(c[2]) && c[2].state == FailoverState::Sospecha);
  for (int i = 3; i <= 9; ++i)
    CHECK(isDht(c[i]) && c[i].state == FailoverState::Respaldo);
  CHECK(isDht(c[10]) && c[10].state == FailoverState::Recuperacion);
  CHECK(isDht(c[11]) && c[11].state == FailoverState::Recuperacion);
  CHECK(isBme(c[12]) && c[12].state == FailoverState::Principal);  // same cycle
  // K=5: attempts only at cycles 5 and 10 (two probes 0x77/0x76 each).
  for (int i = 1; i <= 12; ++i) {
    const bool attempt_cycle = (i == 5 || i == 10);
    CHECK(c[i].begin_calls == (attempt_cycle ? 2 : 0));
  }
  CHECK(countOf(c[5].log, "[BME680] reinit attempt=1 result=fail addr=0x00") == 1);
  CHECK(countOf(c[10].log, "[BME680] reinit attempt=2 result=ok addr=0x76") == 1);
  CHECK(!g_fake_bme.heater_enabled && g_fake_bme.heater_off_calls == 1);
  // [SOURCE] only on changes.
  CHECK(countOf(c[1].log, "[SOURCE] PRINCIPAL->SOSPECHA reason=bme_fail fail=1 ok=0") == 1);
  CHECK(countOf(c[2].log, "[SOURCE]") == 0);
  CHECK(countOf(c[3].log, "[SOURCE] SOSPECHA->RESPALDO reason=bme_degraded fail=3 ok=0") == 1);
  for (int i = 4; i <= 9; ++i) CHECK(countOf(c[i].log, "[SOURCE]") == 0);
  CHECK(countOf(c[10].log, "[SOURCE] RESPALDO->RECUPERACION reason=bme_recovering fail=0 ok=1") == 1);
  CHECK(countOf(c[11].log, "[SOURCE]") == 0);
  CHECK(countOf(c[12].log, "[SOURCE] RECUPERACION->PRINCIPAL reason=bme_recovered fail=0 ok=0") == 1);
}

void testOperationalFailure() {  // cases 12, 13 (read failure in operation)
  resetFakes();
  boot();
  Cycle c[16];
  c[1] = runCycle();
  CHECK(isBme(c[1]));
  g_fake_bme.present_address = 0;  // disconnected at cycle n = 2
  for (int i = 2; i <= 15; ++i) c[i] = runCycle();
  CHECK(c[2].state == FailoverState::Sospecha && c[3].state == FailoverState::Sospecha);
  CHECK(c[4].state == FailoverState::Respaldo);
  for (int i = 2; i <= 15; ++i) {
    const bool attempt_cycle = (i == 5 || i == 10 || i == 15);  // n+3, n+8, n+13
    CHECK(c[i].begin_calls == (attempt_cycle ? 2 : 0));
    CHECK(isDht(c[i]));
  }
}

void testRespondsButInvalid() {  // case 13 (init ok, readings invalid)
  resetFakes();
  g_fake_bme.next_temperature = NAN;
  boot();
  Cycle c[15];
  for (int i = 1; i <= 14; ++i) c[i] = runCycle();
  CHECK(c[3].state == FailoverState::Respaldo);
  for (int i = 1; i <= 14; ++i) {
    const bool attempt_cycle = (i == 4 || i == 9 || i == 14);
    CHECK(c[i].begin_calls == (attempt_cycle ? 2 : 0));  // probes 0x77 then 0x76
    CHECK(isDht(c[i]));
    CHECK(c[i].state != FailoverState::Principal &&
          c[i].state != FailoverState::Recuperacion);
  }
  // Each successful begin() is followed by setGasHeater(0, 0).
  CHECK(!g_fake_bme.heater_enabled);
}

void testInitConfigFailure() {  // case 13 (initialization failure)
  resetFakes();
  g_fake_bme.config_ok = false;
  boot();
  const Cycle c1 = runCycle();
  CHECK(c1.state == FailoverState::Sospecha && isDht(c1));
  CHECK(countOf(c1.log, "[BME680] Read failed (sensor not initialized)") == 1);
}

void testRecoveryWithDhtFailing() {  // case 9
  resetFakes();
  g_fake_bme.present_address = 0;
  boot();
  for (int i = 1; i <= 4; ++i) runCycle();  // RESPALDO
  g_fake_bme.present_address = 0x76;
  const Cycle c5 = runCycle();  // attempt at cycle 5 succeeds
  CHECK(c5.state == FailoverState::Recuperacion && isDht(c5));
  g_fake_dht.temperature = NAN;
  const Cycle c6 = runCycle();
  CHECK(c6.state == FailoverState::Recuperacion && isBme(c6));
  CHECK(countOf(c6.log, "[SOURCE] RECUPERACION->RECUPERACION reason=dht_failed fail=0 ok=2") == 1);
  const Cycle c7 = runCycle();
  CHECK(c7.state == FailoverState::Principal && isBme(c7));
}

void testBothFail() {  // cases 10, 17
  resetFakes();
  g_fake_bme.present_address = 0;
  g_fake_dht.temperature = NAN;
  boot();
  const Cycle c1 = runCycle();
  CHECK(c1.source == SensorSource::None && c1.reading_count == 0);
  CHECK(c1.state == FailoverState::SinSensor);
  CHECK(countOf(c1.log, "[SOURCE] PRINCIPAL->SIN_SENSOR reason=both_failed") == 1);
  CHECK(countOf(c1.log, "[FRAME] Not generated") == 1);
  const Cycle c2 = runCycle();
  CHECK(c2.source == SensorSource::None && countOf(c2.log, "[SOURCE]") == 0);
  g_fake_dht.temperature = 30.10f;
  const Cycle c3 = runCycle();  // fail=3 -> degraded, DHT back
  CHECK(c3.state == FailoverState::Respaldo && isDht(c3));
  CHECK(countOf(c3.log, "[SOURCE] SIN_SENSOR->RESPALDO reason=source_restored") == 1);
}

}  // namespace

int main() {
  testBootWithBme();
  testBootWithoutBmeAndReconnect();
  testOperationalFailure();
  testRespondsButInvalid();
  testInitConfigFailure();
  testRecoveryWithDhtFailing();
  testBothFail();
  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
