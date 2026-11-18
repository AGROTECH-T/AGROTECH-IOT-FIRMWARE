// Host test double for Wire (BUILD-IOT-SENSOR-FAILOVER-05).
#ifndef AGROTECH_FAILOVER_MOCK_WIRE_H
#define AGROTECH_FAILOVER_MOCK_WIRE_H

class TwoWire {
 public:
  int begin_calls = 0;
  bool begin_result = true;
  bool begin(int, int) {
    ++begin_calls;
    return begin_result;
  }
};

extern TwoWire Wire;

#endif
