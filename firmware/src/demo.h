// Demo mode: generates realistic 3-phase Kaifa telegrams (with a valid CRC)
// so the whole system can be tested on the desk without a meter.
#pragma once
#include <stddef.h>
#include <stdint.h>

class DemoMeter {
 public:
  // local_* describe the local time of the telegram (for the load profile and
  // the 0-0:1.0.0 timestamp). dt_s = seconds since the previous telegram.
  // Returns the telegram length written to buf (0 if buf is too small).
  size_t generate(char *buf, size_t size, int year, int mon, int mday, int hour, int min,
                  int sec, bool dst, float dt_s);

 private:
  float rnd();  // 0..1
  uint32_t seed_ = 12345;
  double imp_kWh_ = 12345.678;
  double imp2_kWh_ = 0.0;
  double exp_kWh_ = 0.0;
  float kettle_s_ = 0, ac_s_ = 0, oven_s_ = 0;
};
