// End-to-end simulation on the PC: demo telegrams -> parser -> minute/hour/day
// records, exactly like the firmware, for N days ending now. Writes
// minute.bin / hour.bin / day.bin (52-byte records) for tools/mock_server.py,
// and checks that the energy in every tier adds up to the meter counters.
//   ./simulate <out_dir> [days=400] [discount night 23-7 %=20]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "../../src/demo.h"
#include "../../src/p1_parser.h"
#include "../../src/records.h"
#include "../../src/tariff.h"

static uint32_t dayStart(uint32_t ts) {
  time_t t = ts;
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
  lt.tm_isdst = -1;
  return (uint32_t)mktime(&lt);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: simulate <out_dir> [days]\n"); return 2; }
  setenv("TZ", "IST-2IDT,M3.4.4/26,M10.5.0", 1);
  tzset();
  const int days = argc > 2 ? atoi(argv[2]) : 400;
  TariffSettings tariff;
  tariff.price_u = 635200;
  tariff.rules[0].enabled = 1;
  tariff.rules[0].start_hour = 23;
  tariff.rules[0].end_hour = 7;
  tariff.rules[0].pct_x10 = argc > 3 ? atoi(argv[3]) * 10 : 200;

  const uint32_t now = (uint32_t)time(nullptr);
  const uint32_t start = dayStart(now - days * 86400u);
  const int step = 10;  // one telegram every 10 s keeps the run fast
  // A 7 minute "reboot" every 9 days tests the gap handling
  DemoMeter demo;
  P1Parser parser;
  MinuteAgg minute;
  RecAgg hour(true), day(true);
  uint32_t minute_ts = 0, hour_ts = 0, day_ts = 0;
  Rec prev;
  bool have_prev = false;
  std::vector<Rec> mins, hours, dayrecs;
  uint32_t first_counter = 0, last_counter = 0;
  char buf[1500];

  auto price = [&](uint32_t ts) {
    time_t t = ts;
    struct tm lt;
    localtime_r(&t, &lt);
    return priceAt(tariff, lt.tm_wday, lt.tm_hour);
  };
  auto closeMinute = [&]() {
    if (minute.empty()) return;
    Rec r = minute.finish(minute_ts, have_prev ? &prev : nullptr, price(minute_ts), true);
    minute.clear();
    const uint32_t h = r.ts - r.ts % 3600, d = dayStart(r.ts);
    if (hour_ts && h != hour_ts) { hours.push_back(hour.finish(hour_ts, price(hour_ts))); hour.clear(); if (have_prev) hour.seed(prev); }
    if (day_ts && d != day_ts) { dayrecs.push_back(day.finish(day_ts, tariff.price_u)); day.clear(); if (have_prev) day.seed(prev); }
    hour_ts = h;
    day_ts = d;
    hour.add(r);
    day.add(r);
    mins.push_back(r);
    prev = r;
    have_prev = true;
  };

  for (uint32_t ts = start; ts < now; ts += step) {
    const bool rebooting = (ts - start) % (9 * 86400) < 420 && ts - start > 86400;
    time_t t = ts;
    struct tm lt;
    localtime_r(&t, &lt);
    size_t n = demo.generate(buf, sizeof(buf), lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                             lt.tm_hour, lt.tm_min, lt.tm_sec, lt.tm_isdst > 0, (float)step);
    bool got = false;
    for (size_t i = 0; i < n; i++) got |= parser.feed((uint8_t)buf[i]);
    if (!got) { fprintf(stderr, "telegram rejected at %u\n", ts); return 1; }
    const MeterReading &r = parser.reading();
    if (!first_counter) first_counter = r.importTotalWh();
    last_counter = r.importTotalWh();
    if (rebooting) { minute.clear(); minute_ts = 0; continue; }  // device off: meter keeps counting
    const uint32_t m = ts - ts % 60;
    if (minute_ts && m != minute_ts) closeMinute();
    minute_ts = m;
    minute.add(r);
  }
  closeMinute();

  // Checks: energy per tier vs the meter counters
  auto total = [](const std::vector<Rec> &v) { uint64_t s = 0; for (auto &r : v) s += r.imp_Wh; return s; };
  const uint64_t meter_wh = last_counter - first_counter;
  const uint64_t hours_wh = total(hours) + hour.finish(hour_ts, 0).imp_Wh;
  const uint64_t days_wh = total(dayrecs) + day.finish(day_ts, 0).imp_Wh;
  printf("simulated %d days: %zu minute, %zu hour, %zu day records\n", days, mins.size(), hours.size(), dayrecs.size());
  printf("meter %.3f kWh | minutes %.3f | hours %.3f | days %.3f\n", meter_wh / 1e3, total(mins) / 1e3, hours_wh / 1e3, days_wh / 1e3);
  int fail = 0;
  // Hours and days include the reboot gaps via the counters: within 10 Wh of the meter
  if (hours_wh + 10 < meter_wh || hours_wh > meter_wh) { printf("FAIL hours do not match the meter\n"); fail = 1; }
  if (days_wh + 10 < meter_wh || days_wh > meter_wh) { printf("FAIL days do not match the meter\n"); fail = 1; }
  // Day records cover days, hour boundaries match local time
  for (auto &d : dayrecs) {
    time_t t = d.ts; struct tm lt; localtime_r(&t, &lt);
    if (lt.tm_hour || lt.tm_min) { printf("FAIL day record not at local midnight\n"); fail = 1; break; }
  }
  uint64_t cost_days = 0, cost_full = 0;
  for (auto &d : dayrecs) { cost_days += d.cost_u; cost_full += costMicro(d.imp_Wh, tariff.price_u); }
  printf("cost of finished days: %.2f ILS (%.2f ILS at full price)\n", cost_days / 1e6, cost_full / 1e6);
  if (!(cost_days < cost_full && cost_days > cost_full * 0.75)) { printf("FAIL discount not applied plausibly\n"); fail = 1; }

  // Retention like the device: minutes for the last 5 days
  const uint32_t keep_from = dayStart(now - 4 * 86400u);
  std::vector<Rec> recent;
  for (auto &r : mins) if (r.ts >= keep_from) recent.push_back(r);
  auto write = [&](const char *name, const std::vector<Rec> &v) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", argv[1], name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(v.data(), sizeof(Rec), v.size(), f);
    fclose(f);
  };
  write("minute.bin", recent);
  write("hour.bin", hours);  // the hour/day in progress live in RAM on the device
  write("day.bin", dayrecs);
  printf("%s\n", fail ? "SIMULATION FAILED" : "simulation OK");
  return fail;
}
