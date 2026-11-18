// Host test double for the Arduino core (BUILD-IOT-SENSOR-FAILOVER-05).
// Only what coordinator/CoordinatorSensors.cpp uses. Output is captured.
#ifndef AGROTECH_FAILOVER_MOCK_ARDUINO_H
#define AGROTECH_FAILOVER_MOCK_ARDUINO_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

#define HEX 16

extern uint32_t g_fake_millis;
inline uint32_t millis() { return g_fake_millis; }

class FakeSerial {
 public:
  std::string out;
  void print(const char* text) { out += text; }
  void print(int value) { out += std::to_string(value); }
  void print(double value, int digits) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    out += buffer;
  }
  void println() { out += "\n"; }
  void println(int value) {
    out += std::to_string(value);
    out += "\n";
  }
  void println(const char* text) {
    out += text;
    out += "\n";
  }
  void println(unsigned value, int base) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), base == HEX ? "%X" : "%u", value);
    out += buffer;
    out += "\n";
  }
  int printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    out += buffer;
    return written;
  }
};

extern FakeSerial Serial;

#endif
