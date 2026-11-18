// Host test double for the DHT sensor library (BUILD-IOT-SENSOR-FAILOVER-05).
#ifndef AGROTECH_FAILOVER_MOCK_DHT_H
#define AGROTECH_FAILOVER_MOCK_DHT_H

#include <stdint.h>

#define DHT11 11

struct FakeDhtState {
  float temperature;  // NAN models a failed read
  float humidity;
  int reads;
};

extern FakeDhtState g_fake_dht;

class DHT {
 public:
  DHT(uint8_t, uint8_t) {}
  void begin() {}
  float readTemperature() {
    ++g_fake_dht.reads;
    return g_fake_dht.temperature;
  }
  float readHumidity() { return g_fake_dht.humidity; }
};

#endif
