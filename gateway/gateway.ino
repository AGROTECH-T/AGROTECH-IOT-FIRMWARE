#include <Arduino.h>
#include <LoRa.h>
#include <MQTT.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/task.h>
#include <mbedtls/x509.h>  // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
#include <time.h>

#include <atomic>

#include <AgrotechLinkTest.h>
#include <AgrotechMqttPayload.h>
#include <AgrotechProtocol.h>
#include <AgrotechSha256.h>

#include <stdio.h>
#include <string.h>

#include "GatewayConfig.h"
#include "GatewayCredentials.h"
#include "GatewayFrameLog.h"
#include "GatewayFrameStore.h"
// After GatewayConfig.h: the diagnostic switch lives in GatewayConfig.local.h.
#include "GatewayDiagnostics.h"

// The MQTT path runs for production (AGROTECH_MQTT_TRANSPORT_ENABLED) or for
// the BUILD-IOT-06 diagnostic. With the diagnostic off this is exactly the
// production flag, so the normal gateway logic is unchanged.
static constexpr bool kMqttPathEnabled =
    AGROTECH_MQTT_TRANSPORT_ENABLED || gateway_diag::kEnabled;
static_assert(!gateway_diag::kEnabled || !AGROTECH_LORA_TRANSPORT_ENABLED,
              "MQTT diagnostic builds must keep the LoRa transport disabled");
static_assert(!gateway_diag::kEnabled || !AGROTECH_LORA_PROFILE_AUTHORIZED,
              "MQTT diagnostic builds must not carry an authorized RF profile");
static_assert(!gateway_diag::kEnabled || !AGROTECH_MQTT_TRANSPORT_ENABLED,
              "MQTT diagnostic builds are separate from production MQTT builds");
static_assert(!gateway_diag::kEnabled || !agrotech_link_test::kEnabled,
              "MQTT diagnostic and LoRa link test cannot be combined");

static WiFiClientSecure network_client;
static MQTTClient mqtt(agrotech::kMqttPayloadMaxBytes);
static uint8_t frame_buffer[agrotech::kMaxFrameBytes];
static char topic[64];
static char client_id[32];
// Sized from GatewayCredentials.h (content limit + NUL). The MQTT password is
// the empty string under the Flespi scheme; MQTTClient::connect() then sends
// the username only (lwmqtt sets no password flag for a zero-length string).
static char gateway_uid[gateway_credentials::kGatewayUidLength + 1] = {};
static char mqtt_username[gateway_credentials::kMqttUsernameLength + 1] = {};
static char mqtt_password[gateway_credentials::kMqttPasswordMaxLength + 1] = {};
static char wifi_ssid[gateway_credentials::kWifiSsidMaxLength + 1] = {};
static char wifi_password[gateway_credentials::kWifiPasswordMaxLength + 1] = {};
static uint32_t last_wifi_attempt = 0;
// ---- MQTT reconnection state (MQTT-RESILIENCE-04) ------------------------
// Owned by the MQTT task only. One non-blocking deadline gates every connect
// attempt; loop() (LoRa reception) never waits on it.
static uint32_t mqtt_next_attempt_ms = 0;
static uint32_t mqtt_network_failures = 0;
static uint32_t mqtt_rejected_failures = 0;
static uint32_t mqtt_connected_since_ms = 0;
static bool mqtt_session_active = false;
static_assert(GATEWAY_MQTT_BACKOFF_INITIAL_MS > 0 &&
                  GATEWAY_MQTT_BACKOFF_INITIAL_MS <= GATEWAY_MQTT_BACKOFF_MAX_MS,
              "MQTT backoff: 0 < INITIAL <= MAX");
static_assert(GATEWAY_MQTT_AUTH_BACKOFF_INITIAL_MS > 0 &&
                  GATEWAY_MQTT_AUTH_BACKOFF_INITIAL_MS <= GATEWAY_MQTT_AUTH_BACKOFF_MAX_MS,
              "MQTT auth backoff: 0 < INITIAL <= MAX");
static_assert(GATEWAY_MQTT_BACKOFF_MAX_MS <= 0x7FFFFFFFUL &&
                  GATEWAY_MQTT_AUTH_BACKOFF_MAX_MS <= 0x7FFFFFFFUL,
              "MQTT backoff deadlines must stay wrap-safe (< 2^31 ms)");
static_assert(GATEWAY_MQTT_STABLE_MS > 0, "GATEWAY_MQTT_STABLE_MS must be > 0");
static_assert(GATEWAY_TLS_HANDSHAKE_TIMEOUT_S > 0,
              "GATEWAY_TLS_HANDSHAKE_TIMEOUT_S must be > 0");
static bool radio_ready = false;
static bool mqtt_transport_ready = false;
static volatile bool mqtt_link_up = false;
static volatile bool gateway_connectivity_error = false;
// Durable store unavailable, or the last FRAME could not be stored.
static volatile bool gateway_store_error = false;
static uint32_t gateway_started_ms = 0;
static uint32_t rx_led_until_ms = 0;
// MVP-IOT-F9c: time of the last valid Frame v1 received over LoRa (0 = none).
static uint32_t last_valid_rx_ms = 0;
static const uint32_t kLoraLinkWindowMs = 180000;  // 3 min = 3 coordinator cycles
static agrotech_link_test::RxStats link_test_stats;

// MQTT-QUEUE-PERSISTENCE-05: the durable frame store (GatewayFrameStore) is
// the single source of truth for the store-and-forward queue; there is no
// separate FreeRTOS queue any more.
static_assert(GATEWAY_MQTT_QUEUE_DEPTH == gateway_frame_log::kLiveCapacity,
              "GATEWAY_MQTT_QUEUE_DEPTH must equal the durable store capacity (16)");
static constexpr gateway_frame_log::Status kStoreOk = gateway_frame_log::Status::Ok;
static std::atomic<uint32_t> mqtt_queue_dropped{0};
static std::atomic<uint32_t> mqtt_publish_failures{0};
static std::atomic<uint32_t> mqtt_published{0};
static uint32_t last_queue_drop_log_ms = 0;
static TaskHandle_t mqtt_task_handle = nullptr;

// ---- BUILD-IOT-06 diagnostic state (unused unless gateway_diag::kEnabled) --
static std::atomic<uint32_t> diag_requested{0};
static std::atomic<uint32_t> diag_acked{0};
static char diag_line[32];
static size_t diag_line_length = 0;
static bool diag_line_overflow = false;
static bool diag_vector_ok = false;

static void updateGatewayLed(uint32_t now) {
  bool on = false;
  const uint32_t elapsed = now - gateway_started_ms;
  if (elapsed < 600) {
    const uint32_t phase = elapsed % 400;
    on = phase < 100 || (phase >= 200 && phase < 300);
  } else if (rx_led_until_ms != 0 &&
             static_cast<int32_t>(rx_led_until_ms - now) > 0) {
    on = true;
  } else if (kMqttPathEnabled && mqtt_link_up && !gateway_store_error) {
    // A store error must stay visible even while MQTT is up.
    // MVP-IOT-F9c: SOLID only when a valid LoRa frame arrived in the last
    // 3 min (coordinator link up); otherwise a slow 1 Hz blink = cloud ready,
    // waiting for the coordinator.
    const bool lora_link_up =
        radio_ready && last_valid_rx_ms != 0 &&
        static_cast<uint32_t>(now - last_valid_rx_ms) < kLoraLinkWindowMs;
    on = lora_link_up ? true : (elapsed % 1000) < 500;
  } else if (!kMqttPathEnabled && radio_ready) {
    // DEMO-02B: LoRa-only build with the receiver ready is the normal state,
    // not an error. One 100 ms pulse every 2 s (the coordinator's NORMAL
    // convention; the error pattern below is its 3-pulse ERROR), so the
    // existing 120 ms RX flash above stays visible.
    on = (elapsed % 2000) < 100;
  } else if (!AGROTECH_LORA_TRANSPORT_ENABLED ||
             gateway_connectivity_error ||
             gateway_store_error ||
             (kMqttPathEnabled &&
              (WiFi.status() == WL_CONNECT_FAILED ||
               WiFi.status() == WL_CONNECTION_LOST)) ||
             (AGROTECH_LORA_TRANSPORT_ENABLED && !radio_ready)) {
    const uint32_t phase = elapsed % 2000;
    on = phase < 100 || (phase >= 200 && phase < 300) ||
         (phase >= 400 && phase < 500);
  } else {
    on = (elapsed % 1000) < 500;
  }
  digitalWrite(AGROTECH_GATEWAY_LED_PIN,
               on ? AGROTECH_GATEWAY_LED_ON_LEVEL
                  : AGROTECH_GATEWAY_LED_OFF_LEVEL);
}

static const time_t kClockSyncedThreshold = 1700000000;

static bool requireConfig(bool condition, const char* message) {
  if (!condition) {
    Serial.print("[CONFIG] ");
    Serial.println(message);
  }
  return condition;
}

static bool validateGatewayConfig() {
  bool ok = true;
  if (kMqttPathEnabled) {
    ok = requireConfig(strlen(MQTT_HOST) > 0, "MQTT_HOST must be set") && ok;
    ok = requireConfig(MQTT_PORT > 0, "MQTT_PORT must be set") && ok;
    ok = requireConfig(strlen(MQTT_CA_CERT) > 0,
                       "MQTT_CA_CERT is required; TLS verification cannot be disabled") && ok;
    ok = requireConfig(strlen(NTP_SERVER_1) > 0, "NTP_SERVER_1 must be set") && ok;
  }
  if (!AGROTECH_LORA_TRANSPORT_ENABLED) return ok;
  ok = requireConfig(AGROTECH_LORA_PROFILE_AUTHORIZED,
                     "shared LoRa profile must be authorized before enabling") &&
       ok;
  ok = requireConfig(agrotechRadioPinsValid(),
                     "shared LoRa pin map is invalid (see AgrotechRadioConfig.h)") &&
       ok;
  ok = requireConfig(agrotechRadioProfileValid(),
                     "shared LoRa RF profile is incomplete or out of range") &&
       ok;
  ok = requireConfig(GATEWAY_WIFI_RETRY_MS > 0,
                     "GATEWAY_WIFI_RETRY_MS must be > 0") &&
       ok;
  return ok;
}

static void haltForever(const char* reason) {
  Serial.print("[FATAL] ");
  Serial.println(reason);
  while (true) {
    delay(1000);
  }
}

// Internal 8-bit heap (no PSRAM on the target board). min_free is the
// low-water mark since boot, so a drop across connect() shows the TLS
// handshake peak.
static gateway_diag::HeapSnapshot takeHeapSnapshot() {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {static_cast<uint32_t>(heap_caps_get_free_size(caps)),
          static_cast<uint32_t>(heap_caps_get_minimum_free_size(caps)),
          static_cast<uint32_t>(heap_caps_get_largest_free_block(caps))};
}

static void logHeap(const char* label, const gateway_diag::HeapSnapshot& h) {
  Serial.printf("[DIAG] heap %-14s free=%lu min_free=%lu largest=%lu\n", label,
                static_cast<unsigned long>(h.free_bytes),
                static_cast<unsigned long>(h.min_free_bytes),
                static_cast<unsigned long>(h.largest_block));
}

static void logMqttTaskStack() {
  if (mqtt_task_handle == nullptr) return;
  // ESP-IDF reports the high-water mark in bytes.
  const uint32_t unused = uxTaskGetStackHighWaterMark(mqtt_task_handle);
  Serial.printf("[DIAG] mqtt task stack size=%u never_used=%lu peak_used=%lu\n",
                static_cast<unsigned>(GATEWAY_MQTT_TASK_STACK_BYTES),
                static_cast<unsigned long>(unused),
                static_cast<unsigned long>(GATEWAY_MQTT_TASK_STACK_BYTES - unused));
}

static void applyRadioParameters() {
  LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  LoRa.setSignalBandwidth(static_cast<long>(LORA_BANDWIDTH_HZ));
  LoRa.setCodingRate4(LORA_CODING_RATE_DENOMINATOR);
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setPreambleLength(static_cast<long>(LORA_PREAMBLE_LENGTH));
  if (LORA_USE_CRC == 1) {
    LoRa.enableCrc();
  } else {
    LoRa.disableCrc();
  }
}

// MVP-IOT-F7b: Wi-Fi event log for diagnosis. Prints only reason codes, the
// assigned IP and credential LENGTHS; never the SSID, password or token.
static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  static uint8_t last_reason = 0;
  static uint32_t last_reason_ms = 0;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[WiFi] associated (channel=%u)\n",
                    static_cast<unsigned>(info.wifi_sta_connected.channel));
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WiFi] got IP ");
      Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      const uint32_t now = millis();
      if (reason != last_reason || static_cast<uint32_t>(now - last_reason_ms) >= 5000) {
        last_reason = reason;
        last_reason_ms = now;
        Serial.printf("[WiFi] disconnected reason=%u (%s) rssi=%d\n",
                      static_cast<unsigned>(reason),
                      WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason)),
                      static_cast<int>(info.wifi_sta_disconnected.rssi));
      }
      break;
    }
    default:
      break;
  }
}

static bool startWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  static bool wifi_diag_logged = false;
  if (!wifi_diag_logged) {
    wifi_diag_logged = true;
    WiFi.onEvent(onWifiEvent);
    Serial.printf("[WiFi] ssid_len=%u pass_len=%u\n",
                  static_cast<unsigned>(strlen(wifi_ssid)),
                  static_cast<unsigned>(strlen(wifi_password)));
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  return false;
}

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - last_wifi_attempt) >=
      GATEWAY_WIFI_RETRY_MS) {
    last_wifi_attempt = now;
    startWifi();
    Serial.println("[WiFi] reconnect requested");
  }
  return false;
}

// ---- MQTT reconnection policy (MQTT-RESILIENCE-04) ------------------------
// Signatures use only built-in types: Arduino's generated prototypes are
// emitted before any type declared in this file.

// Exponential delay for the n-th consecutive failure of one class, capped,
// with uniform jitter in [delay/2, delay] (hardware RNG) so a fleet of
// gateways does not reconnect in lockstep after a broker or network outage.
static uint32_t mqttBackoffDelayMs(uint32_t failures, bool rejected) {
  const uint32_t initial = rejected ? GATEWAY_MQTT_AUTH_BACKOFF_INITIAL_MS
                                    : GATEWAY_MQTT_BACKOFF_INITIAL_MS;
  const uint32_t cap = rejected ? GATEWAY_MQTT_AUTH_BACKOFF_MAX_MS
                                : GATEWAY_MQTT_BACKOFF_MAX_MS;
  uint32_t delay_ms = initial;
  for (uint32_t i = 1; i < failures && delay_ms < cap; ++i) {
    delay_ms = delay_ms > cap / 2 ? cap : delay_ms * 2;
  }
  const uint32_t half = delay_ms / 2;
  return half + esp_random() % (delay_ms - half + 1);
}

// Sets the next non-blocking attempt deadline. Logs codes only: never the
// token, the payload or any credential.
static void scheduleMqttRetry(bool rejected, uint32_t now, const char* reason) {
  uint32_t& failures = rejected ? mqtt_rejected_failures : mqtt_network_failures;
  if (failures < UINT32_MAX) ++failures;
  const uint32_t wait_ms = mqttBackoffDelayMs(failures, rejected);
  mqtt_next_attempt_ms = now + wait_ms;
  gateway_connectivity_error = true;
  mqtt_link_up = false;
  Serial.printf("[MQTT] %s; %s retry #%lu in %lu ms\n", reason,
                rejected ? "slow (rejected)" : "network", static_cast<unsigned long>(failures),
                static_cast<unsigned long>(wait_ms));
}

static void resetMqttBackoffIfStable(uint32_t now) {
  if (static_cast<uint32_t>(now - mqtt_connected_since_ms) < GATEWAY_MQTT_STABLE_MS) return;
  if (mqtt_network_failures == 0 && mqtt_rejected_failures == 0) return;
  mqtt_network_failures = 0;
  mqtt_rejected_failures = 0;
  Serial.println("[MQTT] connection stable; reconnect backoff reset");
}

static const char* connackText(int rc) {
  switch (rc) {
    case LWMQTT_UNACCEPTABLE_PROTOCOL: return "unacceptable protocol";
    case LWMQTT_IDENTIFIER_REJECTED: return "identifier rejected";
    case LWMQTT_SERVER_UNAVAILABLE: return "server unavailable";
    case LWMQTT_BAD_USERNAME_OR_PASSWORD: return "bad username or password";
    case LWMQTT_NOT_AUTHORIZED: return "not authorized";
    default: return "unknown";
  }
}

// Classifies a failed mqtt.connect() from the real library state. Returns
// true for the slow "rejected" schedule, false for the network schedule.
static bool classifyConnectFailure(char* reason, size_t reason_size) {
  const lwmqtt_err_t err = mqtt.lastError();
  if (err == LWMQTT_CONNECTION_DENIED) {
    // CONNACK received with a non-zero return code (lwmqtt_connect()).
    const int rc = static_cast<int>(mqtt.returnCode());
    snprintf(reason, reason_size, "broker refused CONNECT (CONNACK rc=%d %s)", rc,
             connackText(rc));
    return rc != LWMQTT_SERVER_UNAVAILABLE;  // rc=3 is temporary by definition
  }
  if (err == LWMQTT_NETWORK_FAILED_CONNECT) {
    // MQTTClient maps every netClient->connect() failure to this one code.
    // DNS was already checked by the caller; the TLS client keeps the cause
    // of this attempt: -1 = socket/TCP connect failure or handshake timeout,
    // any other negative value = mbedTLS error during the TLS handshake.
    char tls_text[48] = {};
    const int tls_err = network_client.lastError(tls_text, sizeof(tls_text));
    if (tls_err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
      // Without NTP a "not yet valid" certificate can be a clock artefact,
      // so it only counts as a rejection once the clock is synced.
      const bool synced = time(nullptr) > kClockSyncedThreshold;
      snprintf(reason, reason_size, "TLS certificate verification failed (%d)%s", tls_err,
               synced ? "" : ", clock not synced");
      return synced;
    }
    if (tls_err < -1) {
      snprintf(reason, reason_size, "TLS handshake failed (%d %s)", tls_err, tls_text);
      return false;
    }
    snprintf(reason, reason_size, "TCP connect failed or TLS handshake timed out (%d)", tls_err);
    return false;
  }
  // Timeout waiting for CONNACK (-4), read/write failure (-5/-6), unexpected
  // packet (-9), buffer errors: all retried on the network schedule.
  snprintf(reason, reason_size, "MQTT CONNECT failed (mqtt_err=%d)", static_cast<int>(err));
  return false;
}

static bool ensureMqtt() {
  // Do not trust a stale TCP socket after the Wi-Fi link disappears.
  if (!ensureWifi()) {
    mqtt_link_up = false;
    return false;
  }
  const uint32_t now = millis();
  if (mqtt.connected()) {
    resetMqttBackoffIfStable(now);
    mqtt_link_up = true;
    return true;
  }

  if (mqtt_session_active) {
    // An established connection ended: the library closed it after a failed
    // loop()/keep-alive/publish (lastError() keeps that cause) or the peer
    // dropped the socket. A pending frame stays pending (see mqttTask).
    mqtt_session_active = false;
    resetMqttBackoffIfStable(now);
    char reason[48];
    snprintf(reason, sizeof(reason), "connection lost (mqtt_err=%d)",
             static_cast<int>(mqtt.lastError()));
    scheduleMqttRetry(false, now, reason);
  }
  if (static_cast<int32_t>(now - mqtt_next_attempt_ms) < 0) {
    mqtt_link_up = false;
    return false;
  }

  // MVP-IOT-F9d: the first hostByName() after the IP state changes makes the
  // ESP32 Arduino core (NetworkManager::hostByName) call dns_clear_cache()
  // without the lwIP TCPIP core lock. If SNTP still has a DNS lookup pending,
  // its callback runs here and sntp_request() -> udp_new_ip_type() asserts
  // ("Required to lock TCPIP core functionality!"). Waiting for the clock
  // before the FIRST lookup guarantees SNTP has no pending DNS request, and
  // also synchronizes time before the first TLS handshake. Bounded to 20 s;
  // later lookups do not clear the cache while the IP state is unchanged.
  static bool first_lookup_done = false;
  static uint32_t clock_wait_started_ms = 0;
  if (!first_lookup_done) {
    if (time(nullptr) <= kClockSyncedThreshold) {
      if (clock_wait_started_ms == 0) {
        clock_wait_started_ms = now == 0 ? 1 : now;
        Serial.println("[NTP] waiting for clock sync before first DNS/TLS (max 20 s)");
      }
      if (static_cast<uint32_t>(now - clock_wait_started_ms) < 20000) {
        mqtt_link_up = false;
        return false;
      }
      Serial.println("[NTP] clock not synced after 20 s; continuing");
    } else {
      Serial.println("[NTP] clock synced");
    }
    first_lookup_done = true;
  }

  // Resolve first: on a DNS failure the TLS client returns without updating
  // its lastError(), so the cause could not be told apart afterwards. The
  // second lookup inside connect() is served by the lwIP DNS cache.
  IPAddress broker_address;
  if (!WiFi.hostByName(MQTT_HOST, broker_address)) {
    scheduleMqttRetry(false, millis(), "DNS resolution of MQTT_HOST failed");
    return false;
  }

  gateway_diag::HeapSnapshot before = {};
  if (gateway_diag::kEnabled) {
    before = takeHeapSnapshot();
    logHeap("pre-TLS", before);
  }
  const uint32_t connect_started = millis();
  const bool connected = mqtt.connect(client_id, mqtt_username, mqtt_password);
  if (gateway_diag::kEnabled) {
    const gateway_diag::HeapSnapshot after = takeHeapSnapshot();
    logHeap(connected ? "post-TLS" : "post-TLS-fail", after);
    Serial.printf(
        "[DIAG] connect %s in %lu ms; handshake_peak<=%ld B session_held=%ld B\n",
        connected ? "ok" : "FAILED",
        static_cast<unsigned long>(millis() - connect_started),
        static_cast<long>(before.free_bytes) -
            static_cast<long>(after.min_free_bytes),
        static_cast<long>(before.free_bytes) - static_cast<long>(after.free_bytes));
    logMqttTaskStack();
  }

  if (connected) {
    // The backoff counters are NOT reset here: only a connection that stays
    // up for GATEWAY_MQTT_STABLE_MS resets them, so a broker that accepts and
    // then immediately drops the gateway still backs off progressively.
    mqtt_session_active = true;
    mqtt_connected_since_ms = millis();
    gateway_connectivity_error = false;
    mqtt_link_up = true;
    Serial.print("[MQTT] connected as ");
    Serial.println(client_id);
  } else {
    char reason[96];
    const bool rejected = classifyConnectFailure(reason, sizeof(reason));
    scheduleMqttRetry(rejected, millis(), reason);
  }
  return connected;
}

static bool clockSynced(time_t now) {
  return now > kClockSyncedThreshold;
}

static bool formatIso8601Utc(char* out, size_t out_size, time_t value) {
  struct tm parts;
  if (gmtime_r(&value, &parts) == nullptr) {
    return false;
  }
  return strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &parts) > 0;
}

static void logQueueDrop(const char* reason, uint32_t now) {
  if (static_cast<uint32_t>(now - last_queue_drop_log_ms) < 5000) return;
  last_queue_drop_log_ms = now;
  Serial.printf(
      "[MQTT] %s; frame dropped total=%lu live=%lu capacity=%u store=%s\n", reason,
      static_cast<unsigned long>(mqtt_queue_dropped.load()),
      static_cast<unsigned long>(gateway_frame_store::liveCount()),
      static_cast<unsigned>(GATEWAY_MQTT_QUEUE_DEPTH), gateway_frame_store::stateText());
}

// A frame is accepted only once its FRAME record has been written to flash
// and verified (GatewayFrameStore). Store unavailable, write/verify failure
// or 16 live frames => the frame is rejected, counted and never published;
// the stored frames are never overwritten (the newest frame is the one lost).
static bool enqueueFrame(const uint8_t* frame, size_t frame_length,
                         time_t rx_time, int rssi, float snr) {
  if (!mqtt_transport_ready) {
    ++mqtt_queue_dropped;
    logQueueDrop("transport not ready", millis());
    return false;
  }
  if (frame == nullptr || frame_length == 0 ||
      frame_length > agrotech::kMaxFrameBytes) {
    ++mqtt_queue_dropped;
    logQueueDrop("invalid frame", millis());
    return false;
  }

  const gateway_frame_log::Status status = gateway_frame_store::store(
      frame, frame_length, static_cast<int64_t>(rx_time), rssi, snr,
      clockSynced(rx_time), !mqtt_link_up, millis());
  if (status == kStoreOk) {
    gateway_store_error = false;
    return true;
  }
  ++mqtt_queue_dropped;
  // A full queue is back-pressure, not a store fault.
  if (status != gateway_frame_log::Status::Full) gateway_store_error = true;
  logQueueDrop(gateway_frame_log::statusText(status), millis());
  return false;
}

static bool buildQueuedPayload(const gateway_frame_log::LiveFrame& item,
                               char* payload, size_t payload_size) {
  char iso_time[32];
  const char* iso_time_pointer = nullptr;
  bool synced = item.time_synced_at_rx;
  if (synced) {
    if (formatIso8601Utc(iso_time, sizeof(iso_time),
                         static_cast<time_t>(item.rx_time))) {
      iso_time_pointer = iso_time;
    } else {
      synced = false;
    }
  }

  agrotech::MqttPayloadInput input;
  input.frame = item.bytes;
  input.frame_length = item.length;
  input.lora_rx_at_iso = iso_time_pointer;
  input.clock_synced = synced;
  input.buffered = item.buffered;
  input.rssi = item.rssi;
  input.snr = item.snr;
  return agrotech::buildMqttPayload(input, payload, payload_size);
}

// Publishes an already frozen payload. A QoS 1 timeout is ambiguous (the
// broker may have stored the message while the PUBACK was lost), so every
// retry must resend byte-identical JSON: the backend deduplicates receipts by
// SHA-256(topic+payload) and ingestion compares a hash that includes
// `buffered`. Rebuilding with a different `buffered` would turn a harmless
// DUPLICATE into a metadata CONFLICT.
static void logDiagPayloadDigest(const char* payload) {
  // SHA-256 of the exact JSON = IotMqttDelivery.payload_sha256 in the backend.
  // Not secret: the payload carries only the public test vector.
  uint8_t digest[32];
  agrotech::Sha256 sha;
  sha.update(reinterpret_cast<const uint8_t*>(payload), strlen(payload));
  sha.finish(digest);
  Serial.print("[DIAG] payload_sha256=");
  for (uint8_t b : digest) Serial.printf("%02x", b);
  Serial.println();
}

static bool publishFrozenPayload(const char* payload, bool buffered) {
  const uint32_t publish_started = millis();
  if (gateway_diag::kEnabled) {
    Serial.printf("[DIAG] publish QoS1 topic=%s bytes=%lu buffered=%s\n", topic,
                  static_cast<unsigned long>(strlen(payload)),
                  buffered ? "true" : "false");
    logDiagPayloadDigest(payload);
  }
  if (!mqtt.publish(topic, payload, false, 1)) {
    if (gateway_diag::kEnabled) {
      Serial.printf("[DIAG] NO PUBACK after %lu ms (error=%d); will resend same bytes\n",
                    static_cast<unsigned long>(millis() - publish_started),
                    static_cast<int>(mqtt.lastError()));
    }
    ++mqtt_publish_failures;
    mqtt_link_up = false;
    gateway_connectivity_error = true;
    Serial.printf("[MQTT] publish failed, error=%d failures=%lu\n",
                  static_cast<int>(mqtt.lastError()),
                  static_cast<unsigned long>(mqtt_publish_failures.load()));
    return false;
  }

  ++mqtt_published;
  mqtt_link_up = true;
  gateway_connectivity_error = false;
  if (gateway_diag::kEnabled) {
    // lwmqtt's QoS 1 publish returns true only after the matching PUBACK.
    ++diag_acked;
    Serial.printf("[DIAG] PUBACK ok packet_id=%u latency_ms=%lu acked=%lu/%lu\n",
                  static_cast<unsigned>(mqtt.lastPacketID()),
                  static_cast<unsigned long>(millis() - publish_started),
                  static_cast<unsigned long>(diag_acked.load()),
                  static_cast<unsigned long>(diag_requested.load()));
    logHeap("post-PUBACK", takeHeapSnapshot());
    logMqttTaskStack();
  }
  Serial.printf(
      "[MQTT] published bytes=%lu buffered=%s live=%lu total=%lu\n",
      static_cast<unsigned long>(strlen(payload)),
      buffered ? "true" : "false",
      static_cast<unsigned long>(gateway_frame_store::liveCount()),
      static_cast<unsigned long>(mqtt_published.load()));
  return true;
}

// Single owner of Wi-Fi reconnection, TLS, lwmqtt, payload construction and
// every flash maintenance step of the durable store. Runs on core 0 so
// connect()/publish() timeouts never block LoRa reception in loop().
//
// Frame life cycle (MQTT-QUEUE-PERSISTENCE-05):
//   RECEIVED (loop) -> STORED (FRAME verified) -> FROZEN (2 FREEZE copies
//   verified) -> INFLIGHT (publish) -> ACKED (PUBACK, then ACK verified)
//   -> RECLAIMED (its sector erased by maintain() once no live frame is left).
// The head frame is only read (peekHead), never removed, until its ACK
// record is durable; every retry, before or after a reboot, rebuilds the
// same bytes from the stored fields and the stored `buffered` decision.
static void mqttTask(void*) {
  gateway_frame_log::LiveFrame head;
  static char payload[agrotech::kMqttPayloadMaxBytes];
  uint32_t built_seq = 0;   // frame whose frozen payload is in `payload`
  uint32_t puback_seq = 0;  // PUBACK received, ACK record not yet durable
  uint32_t last_block_log_ms = 0;

  for (;;) {
    // Sector erase/prepare, recovered-frame FREEZE and reclaim happen only
    // here: never in loop() and never during a network call.
    gateway_frame_store::maintain();

    if (puback_seq != 0) {
      // The PUBACK is known: retry only the ACK record, do not republish.
      // If the device reboots first, the frame is resent (same bytes) and
      // the backend deduplicates it.
      if (gateway_frame_store::acknowledge(puback_seq) != kStoreOk) {
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }
      puback_seq = 0;
      built_seq = 0;
      continue;
    }

    if (!ensureMqtt()) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (!mqtt.loop()) {
      mqtt_link_up = false;
      gateway_connectivity_error = true;
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    mqtt_link_up = true;

    if (!gateway_frame_store::peekHead(&head)) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (head.freeze_unknown) {
      // Both FREEZE copies may have been lost after a publish: guessing
      // `buffered` could produce a conflicting payload. Hold the queue.
      const uint32_t now = millis();
      if (last_block_log_ms == 0 || static_cast<uint32_t>(now - last_block_log_ms) >= 30000) {
        last_block_log_ms = now == 0 ? 1 : now;
        Serial.printf("[STORE] head frame seq=%lu blocked: FREEZE state unknown; "
                      "queue held, operator action required\n",
                      static_cast<unsigned long>(head.seq));
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!head.frozen || head.freeze_copies < 2) {
      // Decide `buffered` once, right before the first publish attempt:
      // true if the frame waited behind others, across an outage or a
      // reboot, or longer than the immediate-forwarding window. A decision
      // already stored is kept as is.
      const bool buffered =
          head.frozen ? head.buffered
                      : (head.recovered || head.buffered_hint ||
                         gateway_frame_store::liveCount() > 1 ||
                         static_cast<uint32_t>(millis() - head.enqueued_ms) >= 1000);
      if (gateway_frame_store::freeze(head.seq, buffered) != kStoreOk) {
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      continue;  // re-read the head with its durable FREEZE state
    }
    if (built_seq != head.seq) {
      if (!buildQueuedPayload(head, payload, sizeof(payload))) {
        // Not reachable for a frame the store accepted (length checked); the
        // frame is kept rather than dropped without a PUBACK.
        Serial.println("[MQTT] payload build failed; head frame held");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      built_seq = head.seq;
    }

    if (!publishFrozenPayload(payload, head.buffered)) {
      // The library already closed the socket; ensureMqtt() detects the lost
      // session, reconnects on the backoff schedule and the same frozen bytes
      // are sent again (the frame stays live until its ACK record exists).
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    // PUBACK with this packet id received: make it durable before the frame
    // leaves the store.
    if (gateway_frame_store::acknowledge(head.seq) != kStoreOk) {
      puback_seq = head.seq;
      Serial.println("[STORE] ACK record not written; frame stays live, ACK retried");
      continue;
    }
    built_seq = 0;
  }
}

static void printLinkTestHex(const uint8_t* data, size_t length) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    if (i > 0) Serial.print(' ');
    Serial.print(kHex[data[i] >> 4]);
    Serial.print(kHex[data[i] & 0x0F]);
  }
}

static void printLinkTestRxStats() {
  Serial.printf(
      "[LT] rx_stats frames=%lu malformed=%lu oversized=%lu incomplete=%lu "
      "lost=%lu duplicates=%lu out_of_order=%lu\n",
      static_cast<unsigned long>(link_test_stats.frames),
      static_cast<unsigned long>(link_test_stats.malformed),
      static_cast<unsigned long>(link_test_stats.oversized),
      static_cast<unsigned long>(link_test_stats.incomplete),
      static_cast<unsigned long>(link_test_stats.lost),
      static_cast<unsigned long>(link_test_stats.duplicates),
      static_cast<unsigned long>(link_test_stats.out_of_order));
}

static void logLinkTestRx(const uint8_t* frame, size_t frame_length,
                          const agrotech::ParsedFrame& parsed, int rssi,
                          float snr, uint32_t dt_ms) {
  const uint32_t tag = agrotech_link_test::tagPrefix(frame, frame_length);
  const size_t expected_length =
      agrotech::frameSize(parsed.fields.reading_count);
  Serial.printf(
      "[LT] rx id=%.13s/b%lu/s%lu/t%08lX bytes=%lu expected=%lu rssi=%d "
      "snr=%.2f dt_ms=%lu hex=",
      parsed.fields.coordinator_uid,
      static_cast<unsigned long>(parsed.fields.boot_counter),
      static_cast<unsigned long>(parsed.fields.seq),
      static_cast<unsigned long>(tag),
      static_cast<unsigned long>(frame_length),
      static_cast<unsigned long>(expected_length), rssi,
      static_cast<double>(snr), static_cast<unsigned long>(dt_ms));
  printLinkTestHex(frame, frame_length);
  Serial.println();
}

// ---- BUILD-IOT-06 diagnostic console ------------------------------------
static bool selfCheckDiagVector() {
  agrotech::ParsedFrame parsed;
  if (agrotech::parseFrame(gateway_diag::kFrame, gateway_diag::kFrameLength,
                           &parsed) != agrotech::Status::Ok) {
    return false;
  }
  return agrotech::verifyFrameTag(gateway_diag::kPublicTestKey,
                                  gateway_diag::kFrame,
                                  gateway_diag::kFrameLength) ==
         agrotech::Status::Ok;
}

static void printDiagHelp() {
  Serial.println("[DIAG] commands: DIAG HELP | DIAG STATUS | DIAG PUBLISH");
}

static void printDiagStatus() {
  const bool clock_ok = clockSynced(time(nullptr));
  Serial.printf(
      "[DIAG] status wifi=%s mqtt=%s clock_synced=%s vector=%s radio=off\n",
      WiFi.status() == WL_CONNECTED ? "up" : "down",
      mqtt_link_up ? "up" : "down", clock_ok ? "true" : "false",
      diag_vector_ok ? "ok" : "INVALID");
  Serial.printf("[DIAG] broker=%s:%u client_id=%s topic=%s\n", MQTT_HOST,
                static_cast<unsigned>(MQTT_PORT), client_id, topic);
  gateway_frame_store::printStatus("DIAG");
  Serial.printf(
      "[DIAG] queue live=%lu capacity=%u item=%u B published=%lu "
      "failures=%lu dropped=%lu requested=%lu/%u acked=%lu\n",
      static_cast<unsigned long>(gateway_frame_store::liveCount()),
      static_cast<unsigned>(GATEWAY_MQTT_QUEUE_DEPTH),
      static_cast<unsigned>(sizeof(gateway_frame_log::LiveFrame)),
      static_cast<unsigned long>(mqtt_published.load()),
      static_cast<unsigned long>(mqtt_publish_failures.load()),
      static_cast<unsigned long>(mqtt_queue_dropped.load()),
      static_cast<unsigned long>(diag_requested.load()),
      static_cast<unsigned>(GATEWAY_MQTT_DIAG_MAX_PUBLISHES),
      static_cast<unsigned long>(diag_acked.load()));
  logHeap("now", takeHeapSnapshot());
  logMqttTaskStack();
}

static void diagInjectVector() {
  if (!diag_vector_ok) {
    Serial.println("[DIAG] refused: embedded vector failed its self-check");
    return;
  }
  if (!mqtt_transport_ready) {
    Serial.println("[DIAG] refused: MQTT path not configured (see [CONFIG])");
    return;
  }
  if (diag_requested.load() >= GATEWAY_MQTT_DIAG_MAX_PUBLISHES) {
    Serial.println("[DIAG] refused: per-boot publish limit reached; reboot to reset");
    return;
  }
  if (!mqtt_link_up) {
    Serial.println("[DIAG] link down: frame will wait in the queue (buffered=true)");
  }
  // Same entry point as a LoRa frame. rx_time=0 keeps lora_rx_at=null and
  // clock_synced=false, so repeated runs give byte-identical payloads.
  if (enqueueFrame(gateway_diag::kFrame, gateway_diag::kFrameLength, 0,
                   gateway_diag::kRssi, gateway_diag::kSnr)) {
    ++diag_requested;
    Serial.printf("[DIAG] official vector queued (%lu/%u)\n",
                  static_cast<unsigned long>(diag_requested.load()),
                  static_cast<unsigned>(GATEWAY_MQTT_DIAG_MAX_PUBLISHES));
  }
}

static void handleDiagCommand(const char* line) {
  if (strcmp(line, "DIAG HELP") == 0) {
    printDiagHelp();
  } else if (strcmp(line, "DIAG STATUS") == 0) {
    printDiagStatus();
  } else if (strcmp(line, "DIAG PUBLISH") == 0) {
    diagInjectVector();
  } else {
    // Never echo input: a mistyped provisioning line could contain secrets.
    Serial.println("[DIAG] unknown command (input not echoed)");
    printDiagHelp();
  }
}

static void pollDiagConsole() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      if (diag_line_overflow) {
        Serial.println("[DIAG] line too long; ignored (input not echoed)");
      } else if (diag_line_length > 0) {
        diag_line[diag_line_length] = '\0';
        handleDiagCommand(diag_line);
      }
      // Wipe so no typed input lingers in RAM.
      memset(diag_line, 0, sizeof(diag_line));
      diag_line_length = 0;
      diag_line_overflow = false;
      continue;
    }
    if (diag_line_length + 1 < sizeof(diag_line)) {
      diag_line[diag_line_length++] = c;
    } else {
      diag_line_overflow = true;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(AGROTECH_GATEWAY_LED_PIN, OUTPUT);
  digitalWrite(AGROTECH_GATEWAY_LED_PIN, AGROTECH_GATEWAY_LED_OFF_LEVEL);
  gateway_started_ms = millis();
  delay(1000);
  Serial.println();
  Serial.println("AGROTECH Gateway MVP - LoRa frame v1");
  Serial.print("[LoRa] profile revision=");
  Serial.print(AGROTECH_LORA_PROFILE_REVISION);
  Serial.print(" signature=0x");
  Serial.println(agrotechRadioProfileSignature(), HEX);
  Serial.print("[LoRa] activation: ");
  Serial.println(agrotechRadioCanActivate()
                     ? "allowed"
                     : agrotechRadioActivationBlockReason());
  if (agrotech_link_test::kEnabled) {
    Serial.println("[LT] link test mode ENABLED");
    if (!agrotechRadioCanActivate()) {
      Serial.print("[LT] radio not ready: ");
      Serial.println(agrotechRadioActivationBlockReason());
    }
  } else {
    Serial.println("[LT] link test mode disabled");
  }

  const bool config_valid = validateGatewayConfig();
  if (!config_valid) {
    Serial.println("[CONFIG] incomplete; transports remain disabled");
  }
  // DEMO-02B: the gateway UID, MQTT token and Wi-Fi credentials are used only
  // by the MQTT path (production MQTT or the BUILD-IOT-06 diagnostic). Without
  // it, NVS is not opened and no SET is requested, so a LoRa-only build reaches
  // the radio initialization; stored credentials are left untouched. With it,
  // loading, validation, provisioning and the halt on storage failure are
  // exactly as before, in the same order.
  if (kMqttPathEnabled) {
    if (!gateway_credentials::loadOrProvision(
            gateway_uid, sizeof(gateway_uid), mqtt_username, sizeof(mqtt_username),
            mqtt_password, sizeof(mqtt_password), wifi_ssid, sizeof(wifi_ssid),
            wifi_password, sizeof(wifi_password)))
      haltForever("credential storage unavailable");
    snprintf(topic, sizeof(topic), "agrotech/v1/gw/%s/up", gateway_uid);
    snprintf(client_id, sizeof(client_id), "gw-%s", gateway_uid);
  } else {
    Serial.println("[PROVISION] skipped: MQTT path disabled; gateway network credentials not required");
  }

  if (gateway_diag::kEnabled) {
    Serial.println("[DIAG] MQTT diagnostic mode ENABLED; LoRa radio forced off");
    diag_vector_ok = selfCheckDiagVector();
    Serial.printf("[DIAG] official Frame v1 vector self-check: %s (%u bytes)\n",
                  diag_vector_ok ? "ok" : "FAILED",
                  static_cast<unsigned>(gateway_diag::kFrameLength));
    logHeap("boot", takeHeapSnapshot());
  }

  if (!gateway_diag::kEnabled && agrotechRadioCanActivate()) {
    SPI.begin(static_cast<int8_t>(LORA_PIN_SCK), static_cast<int8_t>(LORA_PIN_MISO),
              static_cast<int8_t>(LORA_PIN_MOSI), static_cast<int8_t>(LORA_PIN_SS));
    LoRa.setSPI(SPI);
    LoRa.setPins(LORA_PIN_SS, LORA_PIN_RESET, LORA_PIN_DIO0);
    if (LoRa.begin(static_cast<long>(LORA_FREQUENCY_HZ))) {
      applyRadioParameters();
      LoRa.receive();
      radio_ready = true;
    } else {
      Serial.println("[LoRa] radio initialization failed; receiver disabled");
    }
  }

  if (kMqttPathEnabled) {
    if (!config_valid) {
      gateway_connectivity_error = true;
      Serial.println("[MQTT] invalid config; transport stays disabled");
    } else {
      network_client.setCACert(MQTT_CA_CERT);
      network_client.setHandshakeTimeout(GATEWAY_TLS_HANDSHAKE_TIMEOUT_S);
      configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
      mqtt.begin(MQTT_HOST, MQTT_PORT, network_client);
      mqtt.setKeepAlive(30);
      mqtt.setTimeout(5000);
      mqtt.setCleanSession(true);

      // Durable frame store (MQTT-QUEUE-PERSISTENCE-05): recovers the queue
      // from flash, or initializes a completely erased partition. Diagnostic
      // builds get an explicit volatile RAM store and never touch the flash.
      // If the store is unavailable the gateway stays in a safe state: MQTT
      // runs, but no frame is accepted or published (no RAM substitute).
      const gateway_diag::HeapSnapshot before_queue = takeHeapSnapshot();
      if (!gateway_frame_store::begin(gateway_diag::kEnabled)) gateway_store_error = true;
      const gateway_diag::HeapSnapshot after_queue = takeHeapSnapshot();

      mqtt_transport_ready = true;
      last_wifi_attempt = millis() - GATEWAY_WIFI_RETRY_MS;
      mqtt_next_attempt_ms = millis();  // first attempt as soon as Wi-Fi is up
      startWifi();
      if (xTaskCreatePinnedToCore(mqttTask, "mqtt", GATEWAY_MQTT_TASK_STACK_BYTES,
                                  nullptr, 1, &mqtt_task_handle, 0) != pdPASS) {
        haltForever("MQTT task creation failed");
      }
      if (gateway_diag::kEnabled) {
        const gateway_diag::HeapSnapshot after_task = takeHeapSnapshot();
        Serial.printf(
            "[DIAG] store heap=%lu B (%u x %u B live frames, RAM region) task heap=%lu B "
            "(stack %u B + TCB)\n",
            static_cast<unsigned long>(before_queue.free_bytes - after_queue.free_bytes),
            static_cast<unsigned>(GATEWAY_MQTT_QUEUE_DEPTH),
            static_cast<unsigned>(sizeof(gateway_frame_log::LiveFrame)),
            static_cast<unsigned long>(after_queue.free_bytes - after_task.free_bytes),
            static_cast<unsigned>(GATEWAY_MQTT_TASK_STACK_BYTES));
        logHeap("after-task", after_task);
      }
      Serial.printf("[MQTT] store-and-forward task started depth=%u\n",
                    static_cast<unsigned>(GATEWAY_MQTT_QUEUE_DEPTH));
    }
  }
  if (!agrotechRadioCanActivate())
    Serial.println("[LoRa] receiver disabled; authorized shared profile required");
  if (!kMqttPathEnabled)
    Serial.println("[MQTT] transport disabled; no network connection attempted");
  if (gateway_diag::kEnabled) printDiagHelp();
  Serial.println("[READY] gateway initialized");
}

void loop() {
  updateGatewayLed(millis());
  if (gateway_diag::kEnabled) pollDiagConsole();

  if (!radio_ready) { delay(10); return; }

  const int packet_size = LoRa.parsePacket();
  if (packet_size <= 0) {
    delay(5);
    return;
  }

  if (packet_size > static_cast<int>(sizeof(frame_buffer))) {
    while (LoRa.available() > 0) {
      LoRa.read();
    }
    if (agrotech_link_test::kEnabled) ++link_test_stats.oversized;
    Serial.println("[LoRa] frame larger than contract buffer; dropped");
    LoRa.receive();
    return;
  }

  size_t frame_length = 0;
  while (LoRa.available() > 0 && frame_length < sizeof(frame_buffer)) {
    frame_buffer[frame_length++] = static_cast<uint8_t>(LoRa.read());
  }

  if (frame_length != static_cast<size_t>(packet_size)) {
    if (agrotech_link_test::kEnabled) ++link_test_stats.incomplete;
    Serial.println("[LoRa] incomplete read; dropped");
    LoRa.receive();
    return;
  }

  agrotech::ParsedFrame parsed_frame;
  if (agrotech::parseFrame(frame_buffer, frame_length, &parsed_frame) !=
      agrotech::Status::Ok) {
    if (agrotech_link_test::kEnabled) {
      ++link_test_stats.malformed;
      Serial.printf("[LT] malformed frame bytes=%lu hex=",
                    static_cast<unsigned long>(frame_length));
      printLinkTestHex(frame_buffer, frame_length);
      Serial.println();
      printLinkTestRxStats();
    }
    Serial.println("[LoRa] malformed Frame v1; dropped");
    LoRa.receive();
    return;
  }

  const int rssi = LoRa.packetRssi();
  const float snr = LoRa.packetSnr();
  const time_t rx_time = time(nullptr);
  rx_led_until_ms = millis() + 120;
  last_valid_rx_ms = millis();  // MVP-IOT-F9c

  if (agrotech_link_test::kEnabled) {
    const uint32_t now_ms = millis();
    const uint32_t dt_ms =
        link_test_stats.frames == 0 ? 0 : now_ms - link_test_stats.last_rx_ms;
    agrotech_link_test::observeRxFrame(
        &link_test_stats, parsed_frame.fields.coordinator_uid,
        parsed_frame.fields.boot_counter, parsed_frame.fields.seq, now_ms);
    logLinkTestRx(frame_buffer, frame_length, parsed_frame, rssi, snr, dt_ms);
    printLinkTestRxStats();
  }

  Serial.print("[LoRa] rx bytes=");
  Serial.print(static_cast<unsigned long>(frame_length));
  Serial.print(" rssi=");
  Serial.print(rssi);
  Serial.print(" snr=");
  Serial.println(snr);

  // The frame, RSSI, SNR and time are already copied, so the radio is put
  // back in receive mode BEFORE the flash write (MQTT-QUEUE-PERSISTENCE-05):
  // the SX1278 can then take the next packet into its FIFO while the FRAME
  // record is written. The write still occupies this task (and flash
  // operations stall both cores briefly); see docs/MQTT-QUEUE-PERSISTENCE-05.
  LoRa.receive();
  if (AGROTECH_MQTT_TRANSPORT_ENABLED)
    enqueueFrame(frame_buffer, frame_length, rx_time, rssi, snr);
}
