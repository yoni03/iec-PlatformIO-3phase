#include "demo.h"

#include <math.h>
#include <stdio.h>

#include "p1_parser.h"

float DemoMeter::rnd() {
  seed_ = seed_ * 1664525u + 1013904223u;
  return (seed_ >> 8) / 16777216.0f;
}

size_t DemoMeter::generate(char *buf, size_t size, int year, int mon, int mday, int hour, int min,
                           int sec, bool dst, float dt_s) {
  // ---- load profile (W per phase) ----
  float l1 = 180, l2 = 90, l3 = 60;            // always-on: router, standby, fridge base
  const float tod = hour + min / 60.0f;
  if (fmodf(tod * 60, 40) < 15) l1 += 110;     // fridge compressor: 15 of every 40 min
  if (tod >= 6.5f && tod < 23.5f) {            // people at home
    l2 += 150 + 120 * sinf(tod / 3.0f);
    l3 += 80;
  }
  if (tod >= 18 && tod < 23) l3 += 300;        // evening lights, TV
  // occasional heavy loads
  if (kettle_s_ <= 0 && tod >= 6 && tod < 23 && rnd() < dt_s / 2400.0f) kettle_s_ = 180;
  if (ac_s_ <= 0 && tod >= 13 && tod < 23 && rnd() < dt_s / 5400.0f) ac_s_ = 2400;
  if (oven_s_ <= 0 && tod >= 17 && tod < 20 && rnd() < dt_s / 7200.0f) oven_s_ = 1800;
  if (kettle_s_ > 0) { l1 += 2100; kettle_s_ -= dt_s; }
  if (ac_s_ > 0) { l2 += 1350; ac_s_ -= dt_s; }
  if (oven_s_ > 0) { l3 += 2400; oven_s_ -= dt_s; }
  l1 *= 0.97f + 0.06f * rnd();
  l2 *= 0.97f + 0.06f * rnd();
  l3 *= 0.97f + 0.06f * rnd();
  const float total = l1 + l2 + l3;
  imp_kWh_ += total * dt_s / 3600000.0;

  const float v1 = 228 + 6 * rnd(), v2 = 229 + 6 * rnd(), v3 = 227 + 6 * rnd();

  int n = snprintf(
      buf, size,
      "/KFM5KAIFA-METER\r\n\r\n"
      "1-3:0.2.8(50)\r\n"
      "0-0:1.0.0(%02d%02d%02d%02d%02d%02d%c)\r\n"
      "0-0:96.1.1(DEMO0001)\r\n"
      "1-0:1.8.11(%010.3f*kWh)\r\n"
      "1-0:1.8.12(%010.3f*kWh)\r\n"
      "1-0:1.8.13(%010.3f*kWh)\r\n"
      "1-0:2.8.11(%010.3f*kWh)\r\n"
      "1-0:2.8.12(000000.000*kWh)\r\n"
      "1-0:2.8.13(000000.000*kWh)\r\n"
      "0-0:96.14.1(01)\r\n"
      "1-0:1.7.0(%06.3f*kW)\r\n"
      "1-0:2.7.0(00.000*kW)\r\n"
      "1-0:21.7.0(%06.3f*kW)\r\n"
      "1-0:41.7.0(%06.3f*kW)\r\n"
      "1-0:61.7.0(%06.3f*kW)\r\n"
      "1-0:32.7.0(%05.1f*V)\r\n"
      "1-0:52.7.0(%05.1f*V)\r\n"
      "1-0:72.7.0(%05.1f*V)\r\n"
      "1-0:31.7.0(%05.1f*A)\r\n"
      "1-0:51.7.0(%05.1f*A)\r\n"
      "1-0:71.7.0(%05.1f*A)\r\n"
      "!",
      year % 100, mon, mday, hour, min, sec, dst ? 'S' : 'W', imp_kWh_, imp2_kWh_, 0.0,
      exp_kWh_, total / 1000, l1 / 1000, l2 / 1000, l3 / 1000, v1, v2, v3, l1 / v1, l2 / v2,
      l3 / v3);
  if (n <= 0 || (size_t)n + 7 > size) return 0;
  uint16_t crc = 0;
  for (int i = 0; i < n; i++) crc = P1Parser::crc16(crc, (uint8_t)buf[i]);
  n += snprintf(buf + n, size - n, "%04X\r\n", crc);
  return (size_t)n;
}
