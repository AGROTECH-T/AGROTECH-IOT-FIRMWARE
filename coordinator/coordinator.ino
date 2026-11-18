#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>

#include <AgrotechLinkTest.h>
#include <AgrotechProtocol.h>

#include <string.h>

#include "CoordinatorConfig.h"
#include "CoordinatorCredentials.h"
#include "CoordinatorCounters.h"
#include "CoordinatorSensors.h"

enum class CoordinatorLedState : uint8_t;

static CoordinatorCounters counters;
static uint8_t frame_buffer[agrotech::kMaxFrameBytes];
static uint32_t last_acquisition_ms = 0;
static uint32_t frames_since_checkpoint = 0;
static char coordinator_uid[agrotech::kUidBytes + 1] = {};
static uint8_t coordinator_hmac_key[agrotech::kHmacKeyBytes] = {};
static uint16_t coordinator_key_version = 0;
static bool radio_ready = false;
static volatile bool tx_pending = false;
// MVP-IOT-F9c: millis() of the last completed LoRa transmission (0 = none).
static volatile uint32_t last_tx_done_ms = 0;
static const uint32_t kLoraTxOkWindowMs = 180000;  // 3 min = 3 cycles
static uint32_t tx_started_ms = 0;
static agrotech_link_test::TxStats link_test_stats;
static uint32_t link_test_frames = 0;
static bool link_test_finished = false;
static bool link_test_final_stats_printed = false;
static bool link_test_timeout_logged = false;

static void haltForever(const char* reason);

static void onRadioTxDone() {
  tx_pending = false;
  last_tx_done_ms = millis();  // MVP-IOT-F9c
  if (agrotech_link_test::kEnabled) ++link_test_stats.done;
}

enum class CoordinatorLedState : uint8_t { Starting, Normal, Degraded, Error };

static const uint32_t kLedPulseMs = 100;
static const uint32_t kLedGapMs = 100;
static const uint32_t kLedPatternPeriodMs = 2000;
static const uint32_t kLedStartupPatternDurationMs =
    2 * kLedPulseMs + kLedGapMs + kLedPulseMs;
static const uint32_t kLedStartupPostPatternPauseMs = 500;
static const uint32_t kLedStartupDurationMs =
    kLedStartupPatternDurationMs + kLedStartupPostPatternPauseMs;

constexpr uint32_t elapsedMillis(uint32_t now, uint32_t since) {
  return now - since;
}

constexpr uint8_t ledPulseCount(CoordinatorLedState state) {
  return state == CoordinatorLedState::Normal
             ? 1
             : state == CoordinatorLedState::Degraded
                   ? 2
                   : state == CoordinatorLedState::Error ? 3 : 0;
}

constexpr bool pulseActiveAt(uint32_t phase_ms, uint8_t pulse_count) {
  return phase_ms < kLedPulseMs ||
         (pulse_count >= 2 && phase_ms >= kLedPulseMs + kLedGapMs &&
          phase_ms < 2 * kLedPulseMs + kLedGapMs) ||
         (pulse_count >= 3 &&
          phase_ms >= 2 * (kLedPulseMs + kLedGapMs) &&
          phase_ms < 3 * kLedPulseMs + 2 * kLedGapMs);
}

constexpr bool ledPatternActiveAt(CoordinatorLedState state,
                                  uint32_t elapsed_ms) {
  return state == CoordinatorLedState::Starting
             ? (elapsed_ms < kLedPulseMs ||
                (elapsed_ms >= kLedPulseMs + kLedGapMs &&
                 elapsed_ms < 2 * kLedPulseMs + kLedGapMs))
             : pulseActiveAt(elapsed_ms % kLedPatternPeriodMs,
                             ledPulseCount(state));
}

// BUILD-IOT-SENSOR-FAILOVER-04: the LED shows the functional failover state,
// not just the sensor of the last cycle. PRINCIPAL -> Normal; SOSPECHA,
// RESPALDO and RECUPERACION -> Degraded; SIN_SENSOR or no frame -> Error.
constexpr CoordinatorLedState stateAfterCycle(
    coordinator_sensor::FailoverState state, bool frame_generated) {
  return !frame_generated || state == coordinator_sensor::FailoverState::SinSensor
             ? CoordinatorLedState::Error
             : state == coordinator_sensor::FailoverState::Principal
                   ? CoordinatorLedState::Normal
                   : CoordinatorLedState::Degraded;
}

constexpr bool startupCompleteAt(uint32_t now, uint32_t started_at) {
  return elapsedMillis(now, started_at) >= kLedStartupDurationMs;
}

// Compile-time checks exercise pattern boundaries, state mapping and millis()
// wraparound without adding runtime work or another test module.
static_assert(ledPatternActiveAt(CoordinatorLedState::Starting, 0),
              "STARTING first flash must be on");
static_assert(!ledPatternActiveAt(CoordinatorLedState::Starting, 100) &&
                  ledPatternActiveAt(CoordinatorLedState::Starting, 200) &&
                  !ledPatternActiveAt(CoordinatorLedState::Starting, 300),
              "STARTING must produce exactly two short flashes");
static_assert(pulseActiveAt(0, 1) && !pulseActiveAt(100, 1) &&
                  pulseActiveAt(200, 2) && !pulseActiveAt(300, 2) &&
                  pulseActiveAt(400, 3) && !pulseActiveAt(500, 3),
              "operational patterns must have 100 ms pulses and gaps");
static_assert(ledPatternActiveAt(CoordinatorLedState::Normal, 2000),
              "NORMAL pattern must repeat every two seconds");
static_assert(
    stateAfterCycle(coordinator_sensor::FailoverState::Principal, true) ==
            CoordinatorLedState::Normal &&
        stateAfterCycle(coordinator_sensor::FailoverState::Sospecha, true) ==
            CoordinatorLedState::Degraded &&
        stateAfterCycle(coordinator_sensor::FailoverState::Respaldo, true) ==
            CoordinatorLedState::Degraded &&
        stateAfterCycle(coordinator_sensor::FailoverState::Recuperacion, true) ==
            CoordinatorLedState::Degraded &&
        stateAfterCycle(coordinator_sensor::FailoverState::SinSensor, false) ==
            CoordinatorLedState::Error &&
        stateAfterCycle(coordinator_sensor::FailoverState::Principal, false) ==
            CoordinatorLedState::Error,
    "failover state must select the matching LED state");
static_assert(elapsedMillis(0x00000010u, 0xfffffff0u) == 0x20u,
              "millis elapsed-time arithmetic must tolerate wraparound");
static_assert(!startupCompleteAt(899, 0) && startupCompleteAt(900, 0) &&
                  startupCompleteAt(0x00000284u, 0xffffff00u),
              "startup completion must use wrap-safe elapsed time");
static_assert(kLedStartupDurationMs - kLedStartupPatternDurationMs == 500,
              "STARTING must include an additional 500 ms off pause");

class CoordinatorLed {
 public:
  void initialize() {
    digitalWrite(COORDINATOR_LED_PIN, COORDINATOR_LED_OFF_LEVEL);
    pinMode(COORDINATOR_LED_PIN, OUTPUT);
    state_ = CoordinatorLedState::Starting;
    pending_state_ = CoordinatorLedState::Error;
    state_started_ms_ = 0;
    cycle_result_known_ = false;
  }

  void startStartup(uint32_t now) {
    state_ = CoordinatorLedState::Starting;
    state_started_ms_ = now;
    cycle_result_known_ = false;
    update(now);
  }

  // `source` is the published source of the cycle (None: no frame); the LED
  // state itself comes from the failover state machine.
  void recordCycle(coordinator::SensorSource source, bool frame_generated,
                   uint32_t now) {
    const CoordinatorLedState next_state = stateAfterCycle(
        currentFailoverState(),
        frame_generated && source != coordinator::SensorSource::None);
    pending_state_ = next_state;
    cycle_result_known_ = true;
    if (state_ == CoordinatorLedState::Starting) {
      if (startupCompleteAt(now, state_started_ms_)) {
        state_ = next_state;
        state_started_ms_ = now;
      }
    } else if (state_ != next_state) {
      state_ = next_state;
      state_started_ms_ = now;
    }
    update(now);
  }

  void update(uint32_t now) {
    if (state_ == CoordinatorLedState::Starting && cycle_result_known_ &&
        startupCompleteAt(now, state_started_ms_)) {
      state_ = pending_state_;
      state_started_ms_ = now;
    }
    // MVP-IOT-F9c: SOLID while the primary sensor is healthy (Normal) and a
    // LoRa transmission completed in the last 3 min. Degraded/Error patterns
    // are kept so sensor problems stay visible. LoRa is one-way: this means
    // "transmitting", not "received by the gateway".
    const uint32_t tx_done = last_tx_done_ms;
    const bool lora_tx_ok =
        radio_ready && tx_done != 0 &&
        static_cast<uint32_t>(now - tx_done) < kLoraTxOkWindowMs;
    const bool on =
        (state_ == CoordinatorLedState::Normal && lora_tx_ok) ||
        ledPatternActiveAt(state_, elapsedMillis(now, state_started_ms_));
    digitalWrite(COORDINATOR_LED_PIN,
                 on ? COORDINATOR_LED_ON_LEVEL : COORDINATOR_LED_OFF_LEVEL);
  }

 private:
  CoordinatorLedState state_ = CoordinatorLedState::Starting;
  CoordinatorLedState pending_state_ = CoordinatorLedState::Error;
  uint32_t state_started_ms_ = 0;
  bool cycle_result_known_ = false;
};

static CoordinatorLed coordinator_led;

static bool initializeCoordinatorConfig() {
  bool ok = true;
  if (COORDINATOR_TX_INTERVAL_MS == 0 ||
      COORDINATOR_SEQ_CHECKPOINT_EVERY == 0) {
    Serial.println("[CONFIG] acquisition interval/checkpoint must be > 0");
    ok = false;
  }
  return ok;
}

static bool initializeRadio() {
  const char *block_reason = agrotechRadioActivationBlockReason();
  if (block_reason != nullptr) {
    Serial.print("[LoRa] disabled: ");
    Serial.println(block_reason);
    return false;
  }
  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_PIN_SS);
  LoRa.setSPI(SPI);
  LoRa.setPins(LORA_PIN_SS, LORA_PIN_RESET, LORA_PIN_DIO0);
  if (!LoRa.begin(static_cast<long>(LORA_FREQUENCY_HZ))) {
    Serial.println("[LoRa] radio initialization failed; transport disabled");
    return false;
  }
  LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  LoRa.setSignalBandwidth(static_cast<long>(LORA_BANDWIDTH_HZ));
  LoRa.setCodingRate4(LORA_CODING_RATE_DENOMINATOR);
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setPreambleLength(static_cast<long>(LORA_PREAMBLE_LENGTH));
  if (LORA_USE_CRC == 1) LoRa.enableCrc(); else LoRa.disableCrc();
  LoRa.onTxDone(onRadioTxDone);
  radio_ready = true;
  Serial.println("[LoRa] configured; transmission enabled by build configuration");
  return true;
}

static void transmitFrame(const uint8_t* frame, size_t frame_length) {
  if (!radio_ready || frame_length < agrotech::kMinFrameBytes ||
      frame_length > agrotech::kMaxFrameBytes) return;
  if (tx_pending || LoRa.beginPacket() == 0) {
    if (agrotech_link_test::kEnabled) ++link_test_stats.busy;
    Serial.println("[LoRa] transmitter busy; frame dropped");
    return;
  }
  const size_t written = LoRa.write(frame, frame_length);
  if (written != frame_length) {
    LoRa.idle();
    if (agrotech_link_test::kEnabled) ++link_test_stats.write_failed;
    Serial.println("[LoRa] frame write incomplete; frame dropped");
    return;
  }
  tx_pending = true;
  if (LoRa.endPacket(true) == 0) {
    tx_pending = false;
    if (agrotech_link_test::kEnabled) ++link_test_stats.start_failed;
    Serial.println("[LoRa] transmit start failed; frame dropped");
    return;
  }
  if (agrotech_link_test::kEnabled) {
    ++link_test_stats.started;
    tx_started_ms = millis();
    link_test_timeout_logged = false;
  }
  Serial.println("[LoRa] Frame v1 queued");
}

static void printLinkTestHex(const uint8_t* data, size_t length) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    if (i > 0) Serial.print(' ');
    Serial.print(kHex[data[i] >> 4]);
    Serial.print(kHex[data[i] & 0x0F]);
  }
}

static void printLinkTestTxStats() {
  Serial.printf(
      "[LT] tx_stats encoded=%lu encode_err=%lu started=%lu done=%lu "
      "busy=%lu write_err=%lu start_err=%lu timeouts=%lu\n",
      static_cast<unsigned long>(link_test_stats.encoded),
      static_cast<unsigned long>(link_test_stats.encode_failed),
      static_cast<unsigned long>(link_test_stats.started),
      static_cast<unsigned long>(link_test_stats.done),
      static_cast<unsigned long>(link_test_stats.busy),
      static_cast<unsigned long>(link_test_stats.write_failed),
      static_cast<unsigned long>(link_test_stats.start_failed),
      static_cast<unsigned long>(link_test_stats.timeouts));
}

static void logLinkTestTx(uint32_t seq, const uint8_t* frame,
                          size_t frame_length) {
  const uint32_t tag =
      agrotech_link_test::tagPrefix(frame, frame_length);
  const uint32_t airtime_us = agrotech_link_test::airtimeUs(
      LORA_SPREADING_FACTOR, LORA_BANDWIDTH_HZ, LORA_CODING_RATE_DENOMINATOR,
      LORA_PREAMBLE_LENGTH, LORA_USE_CRC == 1, frame_length);
  Serial.printf(
      "[LT] tx id=%s/b%lu/s%lu/t%08lX bytes=%lu air_ms=",
      coordinator_uid, static_cast<unsigned long>(counters.bootCounter()),
      static_cast<unsigned long>(seq), static_cast<unsigned long>(tag),
      static_cast<unsigned long>(frame_length));
  if (airtime_us == 0 || !agrotechRadioProfileValid()) {
    Serial.print("n/a");
  } else {
    Serial.print(static_cast<double>(airtime_us) / 1000.0, 2);
  }
  Serial.print(" hex=");
  printLinkTestHex(frame, frame_length);
  Serial.println();
}

static void runLinkTestCycle() {
  if (link_test_finished) return;

  agrotech::FrameFields fields;
  agrotech::initFrameFields(&fields);
  strncpy(fields.coordinator_uid, coordinator_uid, agrotech::kUidBytes);
  fields.coordinator_uid[agrotech::kUidBytes] = '\0';
  fields.key_version = coordinator_key_version;
  fields.boot_counter = counters.bootCounter();

  uint32_t seq = 0;
  if (!counters.takeSeq(&seq)) {
    counters.checkpoint();
    haltForever("sequence exhausted; reprovisioning required");
  }
  fields.seq = seq;

  fields.readings[0].channel = 1;
  fields.readings[0].metric_code = agrotech::kMetricCodeTemperature;
  fields.readings[0].value = agrotech_link_test::kTemperatureX100;
  fields.readings[1].channel = 2;
  fields.readings[1].metric_code = agrotech::kMetricCodeRelativeHumidity;
  fields.readings[1].value = agrotech_link_test::kHumidityX100;
  fields.reading_count = 2;

  size_t frame_length = 0;
  const agrotech::Status status =
      agrotech::encodeFrame(fields, coordinator_hmac_key, frame_buffer,
                            sizeof(frame_buffer), &frame_length);
  if (status != agrotech::Status::Ok) {
    ++link_test_stats.encode_failed;
    Serial.print("[LT] encode failed: ");
    Serial.println(agrotech::statusText(status));
    printLinkTestTxStats();
    return;
  }

  ++link_test_stats.encoded;
  ++link_test_frames;
  logLinkTestTx(seq, frame_buffer, frame_length);
  transmitFrame(frame_buffer, frame_length);
  printLinkTestTxStats();

  ++frames_since_checkpoint;
  if (frames_since_checkpoint >= COORDINATOR_SEQ_CHECKPOINT_EVERY ||
      counters.seqExhausted()) {
    frames_since_checkpoint = 0;
    if (!counters.checkpoint())
      Serial.println(
          "[NVS] seq checkpoint failed; boot_counter remains authoritative");
  }
  if (counters.seqExhausted()) haltForever("sequence exhausted");

  if (agrotech_link_test::kFrameLimit != 0 &&
      link_test_frames >= agrotech_link_test::kFrameLimit) {
    link_test_finished = true;
    Serial.println("[LT] frame limit reached; test scheduling stopped");
  }
}

static void haltForever(const char* reason) {
  Serial.print("[FATAL] ");
  Serial.println(reason);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("AGROTECH Coordinator BUILD-IOT-01 - acquire and encode only");
  Serial.print("[LoRa] profile revision=");
  Serial.print(AGROTECH_LORA_PROFILE_REVISION);
  Serial.print(" signature=0x");
  Serial.println(agrotechRadioProfileSignature(), HEX);
  Serial.print("[LoRa] activation: ");
  Serial.println(agrotechRadioCanActivate()
                     ? "allowed"
                     : agrotechRadioActivationBlockReason());

  coordinator_led.initialize();

  if (!initializeCoordinatorConfig()) haltForever("invalid coordinator config");
  if (!coordinator_credentials::loadOrProvision(
          coordinator_uid, &coordinator_key_version, coordinator_hmac_key))
    haltForever("credential storage unavailable");
  initializeSensors();
  initializeRadio();

  if (!counters.begin()) {
    if (counters.bootExhausted())
      haltForever("boot_counter exhausted; controlled reprovisioning required");
    haltForever("NVS counter initialization failed");
  }
  Serial.print("[NVS] boot_counter=");
  Serial.println(static_cast<unsigned long>(counters.bootCounter()));
  // Make the first loop acquisition due immediately; loop() then timestamps
  // each cycle normally so all subsequent cycles retain the 60-second period.
  last_acquisition_ms = millis() - COORDINATOR_TX_INTERVAL_MS;
  coordinator_led.startStartup(millis());
  if (agrotech_link_test::kEnabled) {
    Serial.println("[LT] link test mode ENABLED");
    if (!radio_ready) {
      Serial.print("[LT] radio not ready: ");
      Serial.println(agrotechRadioActivationBlockReason());
    }
  } else {
    Serial.println("[LT] link test mode disabled");
  }
  Serial.println("[READY] sensor acquisition enabled; LoRa is not activated");
}

void loop() {
  const uint32_t now = millis();
  coordinator_led.update(now);

  if (agrotech_link_test::kEnabled) {
    if (tx_pending && !link_test_timeout_logged &&
        static_cast<uint32_t>(now - tx_started_ms) > 5000) {
      link_test_timeout_logged = true;
      ++link_test_stats.timeouts;
      Serial.println("[LT] tx timeout: TxDone not observed within 5000 ms");
      printLinkTestTxStats();
    }
    if (link_test_finished) {
      if (!tx_pending && !link_test_final_stats_printed) {
        link_test_final_stats_printed = true;
        printLinkTestTxStats();
        Serial.println("[LT] test scheduling complete");
      }
      delay(10);
      return;
    }
    if (static_cast<uint32_t>(now - last_acquisition_ms) <
        COORDINATOR_TX_INTERVAL_MS) {
      delay(10);
      return;
    }
    last_acquisition_ms = now;
    runLinkTestCycle();
    return;
  }

  if (static_cast<uint32_t>(now - last_acquisition_ms) <
      COORDINATOR_TX_INTERVAL_MS) {
    delay(10);
    return;
  }
  last_acquisition_ms = now;

  agrotech::FrameFields fields;
  agrotech::initFrameFields(&fields);
  strncpy(fields.coordinator_uid, coordinator_uid, agrotech::kUidBytes);
  fields.coordinator_uid[agrotech::kUidBytes] = '\0';
  fields.key_version = coordinator_key_version;
  fields.boot_counter = counters.bootCounter();

  const coordinator::SensorSource sensor_source =
      readConfiguredSensors(&fields);
  if (sensor_source == coordinator::SensorSource::None) {
    coordinator_led.recordCycle(sensor_source, false, millis());
    return;
  }

  uint32_t seq = 0;
  if (!counters.takeSeq(&seq)) {
    counters.checkpoint();
    haltForever("sequence exhausted; reprovisioning required");
  }
  fields.seq = seq;

  size_t frame_length = 0;
  const agrotech::Status status =
      agrotech::encodeFrame(fields, coordinator_hmac_key, frame_buffer,
                            sizeof(frame_buffer), &frame_length);
  if (status != agrotech::Status::Ok) {
    Serial.print("[FRAME] encode failed: ");
    Serial.println(agrotech::statusText(status));
    coordinator_led.recordCycle(sensor_source, false, millis());
    return;
  }

  Serial.print("[FRAME] valid Frame v1 generated; boot=");
  Serial.print(static_cast<unsigned long>(fields.boot_counter));
  Serial.print(" seq=");
  Serial.print(static_cast<unsigned long>(fields.seq));
  Serial.print(" readings=");
  Serial.print(fields.reading_count);
  Serial.print(" bytes=");
  Serial.println(static_cast<unsigned long>(frame_length));
  transmitFrame(frame_buffer, frame_length);
  coordinator_led.recordCycle(sensor_source, true, millis());

  ++frames_since_checkpoint;
  if (frames_since_checkpoint >= COORDINATOR_SEQ_CHECKPOINT_EVERY ||
      counters.seqExhausted()) {
    frames_since_checkpoint = 0;
    if (!counters.checkpoint())
      Serial.println("[NVS] seq checkpoint failed; boot_counter remains authoritative");
  }
  if (counters.seqExhausted()) haltForever("sequence exhausted");
}
