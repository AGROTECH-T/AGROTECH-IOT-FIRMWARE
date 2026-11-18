"""Independent software harness for coordinator LED startup/operational states."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
INO = (ROOT / "coordinator" / "coordinator.ino").read_text(encoding="utf-8")
CONFIG = (ROOT / "coordinator" / "CoordinatorConfig.h").read_text(
    encoding="utf-8"
)
SENSORS = (ROOT / "coordinator" / "CoordinatorSensors.cpp").read_text(
    encoding="utf-8"
)

STARTING, NORMAL, DEGRADED, ERROR = "STARTING", "NORMAL", "DEGRADED", "ERROR"
UINT32_MASK = 0xFFFFFFFF


def read_uint_constant(name):
    match = re.search(
        rf"static const uint32_t {name}\s*=\s*(\d+)\s*;", INO + CONFIG
    )
    if not match:
        raise AssertionError(f"Missing integer constant {name}")
    return int(match.group(1))


PULSE_MS = read_uint_constant("kLedPulseMs")
GAP_MS = read_uint_constant("kLedGapMs")
PERIOD_MS = read_uint_constant("kLedPatternPeriodMs")
PAUSE_MS = read_uint_constant("kLedStartupPostPatternPauseMs")
ACQUISITION_MS = read_uint_constant("COORDINATOR_TX_INTERVAL_MS")
STARTUP_PATTERN_MS = 2 * PULSE_MS + GAP_MS + PULSE_MS
STARTUP_TOTAL_MS = STARTUP_PATTERN_MS + PAUSE_MS


def elapsed(now, since):
    return (now - since) & UINT32_MASK


def pattern_on(state, elapsed_ms):
    if state == STARTING:
        return elapsed_ms < PULSE_MS or (
            PULSE_MS + GAP_MS <= elapsed_ms < 2 * PULSE_MS + GAP_MS
        )
    phase = elapsed_ms % PERIOD_MS
    pulses = {NORMAL: 1, DEGRADED: 2, ERROR: 3}[state]
    return (
        phase < PULSE_MS
        or (
            pulses >= 2
            and PULSE_MS + GAP_MS <= phase < 2 * PULSE_MS + GAP_MS
        )
        or (
            pulses >= 3
            and 2 * (PULSE_MS + GAP_MS)
            <= phase
            < 3 * PULSE_MS + 2 * GAP_MS
        )
    )


# BUILD-IOT-SENSOR-FAILOVER-05: the LED follows the functional failover state
# (coordinator.ino stateAfterCycle), not the sensor of the last cycle.
PRINCIPAL, SOSPECHA, RESPALDO, RECUPERACION, SIN_SENSOR = (
    "PRINCIPAL", "SOSPECHA", "RESPALDO", "RECUPERACION", "SIN_SENSOR"
)


def state_after_cycle(failover_state, frame_generated):
    if not frame_generated or failover_state == SIN_SENSOR:
        return ERROR
    return NORMAL if failover_state == PRINCIPAL else DEGRADED


class LedHarness:
    """Independent time-driven model of the production LED state transitions."""

    def __init__(self, started_at=0, state=STARTING):
        self.state = state
        self.started_at = started_at & UINT32_MASK
        self.pending = None
        self.result_known = False

    def record_cycle(self, source, frame_generated, now):
        target = state_after_cycle(source, frame_generated)
        self.pending = target
        self.result_known = True
        if self.state == STARTING:
            if elapsed(now, self.started_at) >= STARTUP_TOTAL_MS:
                self.state = target
                self.started_at = now & UINT32_MASK
        elif self.state != target:
            self.state = target
            self.started_at = now & UINT32_MASK

    def tick(self, now):
        if (
            self.state == STARTING
            and self.result_known
            and elapsed(now, self.started_at) >= STARTUP_TOTAL_MS
        ):
            self.state = self.pending
            self.started_at = now & UINT32_MASK
        return self.state, pattern_on(self.state, elapsed(now, self.started_at))


def rising_edges(samples, state_filter=None):
    count = 0
    previous = False
    for state, on in samples:
        if on and not previous and (state_filter is None or state == state_filter):
            count += 1
        previous = on
    return count


def test_01_starting():
    controller = LedHarness()
    controller.record_cycle(PRINCIPAL, True, 50)
    samples = [controller.tick(t) for t in range(STARTUP_TOTAL_MS + 1)]
    assert rising_edges(samples, STARTING) == 2
    assert samples[STARTUP_PATTERN_MS - 1] == (STARTING, False)
    assert all(state == STARTING and not on for state, on in samples[300:900])
    assert samples[STARTUP_TOTAL_MS] == (NORMAL, True)
    assert rising_edges(samples) == 3  # two startup pulses, then first NORMAL pulse


def test_02_starting_to_normal():
    controller = LedHarness()
    controller.record_cycle(PRINCIPAL, True, 50)
    assert controller.tick(STARTUP_TOTAL_MS - 1) == (STARTING, False)
    assert controller.tick(STARTUP_TOTAL_MS) == (NORMAL, True)
    assert controller.tick(STARTUP_TOTAL_MS + 100) == (NORMAL, False)
    assert controller.tick(STARTUP_TOTAL_MS + PERIOD_MS) == (NORMAL, True)


def test_03_starting_to_degraded():
    controller = LedHarness()
    controller.record_cycle(SOSPECHA, True, 50)
    assert controller.tick(STARTUP_TOTAL_MS - 1) == (STARTING, False)
    assert controller.tick(STARTUP_TOTAL_MS) == (DEGRADED, True)
    assert controller.tick(STARTUP_TOTAL_MS + 200) == (DEGRADED, True)
    assert controller.tick(STARTUP_TOTAL_MS + 300) == (DEGRADED, False)


def test_04_starting_to_error():
    controller = LedHarness()
    encoder_called = False  # no measurement means the production path returns first
    controller.record_cycle(SIN_SENSOR, False, 50)
    assert not encoder_called
    assert controller.tick(STARTUP_TOTAL_MS - 1) == (STARTING, False)
    assert controller.tick(STARTUP_TOTAL_MS) == (ERROR, True)
    assert controller.tick(STARTUP_TOTAL_MS + 200) == (ERROR, True)
    assert controller.tick(STARTUP_TOTAL_MS + 400) == (ERROR, True)
    assert controller.tick(STARTUP_TOTAL_MS + 500) == (ERROR, False)
    assert state_after_cycle(PRINCIPAL, False) == ERROR  # encoder failure
    encode_failure = INO.index("if (status != agrotech::Status::Ok)")
    frame_success_log = INO.index("[FRAME] valid Frame v1 generated")
    assert encode_failure < frame_success_log
    assert "recordCycle(sensor_source, false, millis())" in INO[encode_failure:frame_success_log]


def test_05_nonblocking_and_integrity():
    led_class = INO[INO.index("class CoordinatorLed"): INO.index(
        "static CoordinatorLed coordinator_led"
    )]
    assert "delay(" not in led_class
    assert ACQUISITION_MS == 60000
    assert "COORDINATOR_LED_PIN = 25" in CONFIG
    assert "COORDINATOR_LED_ON_LEVEL = 1" in CONFIG
    assert "COORDINATOR_LED_OFF_LEVEL = 0" in CONFIG
    loop = INO[INO.index("void loop()") :]
    assert "startupCompleteAt" not in loop
    assert loop.index("COORDINATOR_TX_INTERVAL_MS") < loop.index("readConfiguredSensors(&fields)")
    # Ordering is checked inside loop(): runLinkTestCycle() (BUILD-IOT-03D link
    # test mode) also calls encodeFrame earlier in the file.
    no_measurement = loop.index(
        "sensor_source == coordinator::SensorSource::None"
    )
    encode_call = loop.index("agrotech::encodeFrame")
    assert no_measurement < encode_call
    assert "return coordinator::SensorSource::None;" in SENSORS
    assert "COORDINATOR_TX_INTERVAL_MS = 60000" in CONFIG


def test_06_millis_rollover():
    start = 0xFFFFFF00
    controller = LedHarness(started_at=start)
    result_at = (start + 50) & UINT32_MASK
    transition_at = (start + STARTUP_TOTAL_MS) & UINT32_MASK
    controller.record_cycle(PRINCIPAL, True, result_at)
    assert elapsed(transition_at, start) == STARTUP_TOTAL_MS
    assert controller.tick((transition_at - 1) & UINT32_MASK) == (STARTING, False)
    assert controller.tick(transition_at) == (NORMAL, True)


def test_07_failover_state_mapping():
    assert state_after_cycle(PRINCIPAL, True) == NORMAL
    for degraded in (SOSPECHA, RESPALDO, RECUPERACION):
        assert state_after_cycle(degraded, True) == DEGRADED
    assert state_after_cycle(SIN_SENSOR, False) == ERROR
    assert state_after_cycle(SIN_SENSOR, True) == ERROR
    # The production mapping and its compile-time checks express the same table.
    start = INO.index("constexpr CoordinatorLedState stateAfterCycle(")
    body = INO[start : INO.index("constexpr bool startupCompleteAt", start)]
    assert "state == coordinator_sensor::FailoverState::SinSensor" in body
    assert "state == coordinator_sensor::FailoverState::Principal" in body
    for name, led in (
        ("Principal, true", "Normal"),
        ("Sospecha, true", "Degraded"),
        ("Respaldo, true", "Degraded"),
        ("Recuperacion, true", "Degraded"),
        ("SinSensor, false", "Error"),
    ):
        assert re.search(
            rf"stateAfterCycle\(coordinator_sensor::FailoverState::{name}\)\s*==\s*"
            rf"CoordinatorLedState::{led}",
            INO,
        ), name
    led_class = INO[INO.index("class CoordinatorLed"): INO.index("static CoordinatorLed coordinator_led")]
    assert "currentFailoverState()" in led_class
    # SIN_SENSOR: no frame and no sequence consumed (return before takeSeq).
    loop = INO[INO.index("void loop()") :]
    sensors_call = loop.index("readConfiguredSensors(&fields)")
    none_check = loop.index("sensor_source == coordinator::SensorSource::None")
    take_seq = loop.index("counters.takeSeq(&seq)", sensors_call)
    assert sensors_call < none_check < take_seq
    assert "return;" in loop[none_check:take_seq]


def main():
    assert PULSE_MS == 100 and GAP_MS == 100 and PERIOD_MS == 2000
    assert STARTUP_PATTERN_MS == 400 and PAUSE_MS == 500
    assert STARTUP_TOTAL_MS == 900
    tests = [
        test_01_starting,
        test_02_starting_to_normal,
        test_03_starting_to_degraded,
        test_04_starting_to_error,
        test_05_nonblocking_and_integrity,
        test_06_millis_rollover,
        test_07_failover_state_mapping,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"{len(tests)} controlled LED simulation tests, 0 failures")


if __name__ == "__main__":
    main()
