// Unit tests for the parts of the firmware that do not need the ESP32.
// Run on a PC:  make -C firmware/test/host
#include <stdio.h>
#include <string.h>

#include "../../src/demo.h"
#include "../../src/p1_parser.h"
#include "../../src/records.h"
#include "../../src/tariff.h"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    checks++;                                                          \
    if (!(cond)) {                                                     \
      failures++;                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
    }                                                                  \
  } while (0)
#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    checks++;                                                                           \
    long long _a = (long long)(a), _b = (long long)(b);                                 \
    if (_a != _b) {                                                                     \
      failures++;                                                                       \
      printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
    }                                                                                   \
  } while (0)

// Real telegram from a Kaifa MA110M (klimofey/iec-p1-esphome README)
static const char *kSingle =
    "/KFM5KAIFA-METER\r\n\r\n"
    "1-3:0.2.8(50)\r\n"
    "0-0:1.0.0(251207001548W)\r\n"
    "0-0:96.1.1(K06725)\r\n"
    "1-0:1.8.11(000849.710*kWh)\r\n"
    "1-0:1.8.12(000000.000*kWh)\r\n"
    "1-0:1.8.13(003421.613*kWh)\r\n"
    "1-0:2.8.11(000000.000*kWh)\r\n"
    "1-0:2.8.12(000000.000*kWh)\r\n"
    "1-0:2.8.13(000000.000*kWh)\r\n"
    "0-0:96.14.1(03)\r\n"
    "1-0:1.7.0(00.610*kW)\r\n"
    "1-0:2.7.0(00.000*kW)\r\n"
    "0-0:96.13.0()\r\n"
    "1-0:32.7.0(232.0*V)\r\n"
    "1-0:31.7.0(002.9*A)\r\n"
    "1-0:99.1.0(1)(0-0:96.10.7)(1-0:1.29.0)(1-0:2.29.0)(251207001500W)(00)(000000.165*kWh)(000000.000*kWh)\r\n"
    "!05F2\r\n";

static int feedAll(P1Parser &p, const char *s) {
  int complete = 0;
  for (; *s; s++) complete += p.feed((uint8_t)*s);
  return complete;
}

static void testFixed() {
  int64_t v;
  CHECK(P1Parser::parseFixed("000849.710*kWh)", 3, &v)); CHECK_EQ(v, 849710);
  CHECK(P1Parser::parseFixed("00.610*kW", 3, &v)); CHECK_EQ(v, 610);
  CHECK(P1Parser::parseFixed("232.0*V", 1, &v)); CHECK_EQ(v, 2320);
  CHECK(P1Parser::parseFixed("002.9*A", 2, &v)); CHECK_EQ(v, 290);
  CHECK(P1Parser::parseFixed("03)", 0, &v)); CHECK_EQ(v, 3);
  CHECK(P1Parser::parseFixed("1.23456", 3, &v)); CHECK_EQ(v, 1234);
  CHECK(!P1Parser::parseFixed(")", 3, &v));
  // Large counter keeps Wh precision (a float would not)
  CHECK(P1Parser::parseFixed("123456.789*kWh", 3, &v)); CHECK_EQ(v, 123456789);
}

static void testSinglePhase() {
  P1Parser p;
  CHECK_EQ(feedAll(p, kSingle), 1);
  const MeterReading &r = p.reading();
  CHECK_EQ(p.stats().ok, 1);
  CHECK_EQ(p.stats().crc_errors, 0);
  CHECK_EQ(r.imp_Wh[0], 849710);
  CHECK_EQ(r.imp_Wh[2], 3421613);
  CHECK_EQ(r.importTotalWh(), 849710 + 3421613);
  CHECK_EQ(r.p_imp_W, 610);
  CHECK_EQ(r.v_dV[0], 2320);
  CHECK_EQ(r.i_cA[0], 290);
  CHECK_EQ(r.tariff, 3);
  CHECK_EQ(r.phases(), 1);
  CHECK(!(r.present & HAS_PL1));
  CHECK(strcmp(r.meter_time, "251207001548W") == 0);
  CHECK(strcmp(r.meter_id, "K06725") == 0);
  CHECK(strcmp(r.header, "/KFM5KAIFA-METER") == 0);
}

static void testBadCrcAndRecovery() {
  P1Parser p;
  std::string bad = kSingle;
  bad[bad.find("00.610")] = '1';  // corrupt one digit
  CHECK_EQ(feedAll(p, bad.c_str()), 0);
  CHECK_EQ(p.stats().crc_errors, 1);
  // Garbage, then a telegram cut in the middle, then a good one
  std::string cut = std::string(kSingle).substr(0, 200);
  CHECK_EQ(feedAll(p, "\x01\xff garbage\r\n"), 0);
  CHECK_EQ(feedAll(p, cut.c_str()), 0);
  CHECK_EQ(feedAll(p, kSingle), 1);
  CHECK_EQ(p.stats().ok, 1);
  // Two telegrams back to back
  std::string two = std::string(kSingle) + kSingle;
  CHECK_EQ(feedAll(p, two.c_str()), 2);
}

static void testDemoTelegram() {
  DemoMeter demo;
  P1Parser p;
  char buf[1500];
  size_t n = demo.generate(buf, sizeof(buf), 2026, 11, 1, 19, 30, 5, false, 1.0f);
  CHECK(n > 400);
  CHECK_EQ(feedAll(p, buf), 1);
  const MeterReading &r = p.reading();
  CHECK_EQ(r.phases(), 3);
  CHECK(r.present & HAS_PL3);
  CHECK(r.p_imp_W > 300);
  // per-phase power adds up to the total (within rounding)
  long diff = (long)(r.pl_W[0] + r.pl_W[1] + r.pl_W[2]) - (long)r.p_imp_W;
  CHECK(diff >= -3 && diff <= 3);
  CHECK(r.v_dV[1] > 2200);
  CHECK(strcmp(r.meter_time, "261101193005W") == 0);
  // counters move forward over an hour of telegrams
  uint32_t first = r.importTotalWh();
  int ok = 0;
  for (int s = 0; s < 3600; s++) {
    n = demo.generate(buf, sizeof(buf), 2026, 11, 1, 19, s / 60, s % 60, false, 1.0f);
    ok += feedAll(p, buf);
  }
  CHECK_EQ(ok, 3600);
  CHECK(p.reading().importTotalWh() > first + 300);
}

static MeterReading reading(uint32_t imp_Wh, uint32_t p_W, uint16_t v = 2300) {
  MeterReading r;
  r.present = HAS_IMPORT | HAS_POWER | HAS_L1;
  r.imp_Wh[0] = imp_Wh;
  r.p_imp_W = p_W;
  r.v_dV[0] = v;
  r.i_cA[0] = (uint16_t)(p_W * 100 / 230);
  return r;
}

static void testMinuteAgg() {
  MinuteAgg m;
  m.add(reading(1000000, 500));
  m.add(reading(1000005, 700));
  m.add(reading(1000010, 300));
  Rec r = m.finish(1700000040, nullptr, 635200, false);
  CHECK_EQ(r.ts, 1700000040);
  CHECK_EQ(r.p_avg, 500);
  CHECK_EQ(r.p_max, 700);
  CHECK_EQ(r.p_min, 300);
  CHECK_EQ(r.pl_avg[0], 500);  // 1-phase: total goes to L1
  CHECK_EQ(r.imp_end, 1000010);
  CHECK_EQ(r.imp_Wh, 10);      // no previous record: first..last of this minute
  CHECK_EQ(r.cost_u, 6352);    // 10 Wh * 0.6352
  CHECK_EQ(r.samples, 3);
  CHECK(r.flags & REC_COUNTERS);

  // Contiguous previous record: energy counted from its end
  MinuteAgg m2;
  m2.add(reading(1000020, 600));
  m2.add(reading(1000030, 600));
  Rec r2 = m2.finish(1700000100, &r, 635200, false);
  CHECK_EQ(r2.imp_Wh, 20);
  // Not contiguous (gap): only this minute's own counters
  Rec r3 = m2.finish(1700000400, &r, 635200, false);
  CHECK_EQ(r3.imp_Wh, 10);
  // Counter going backwards does not create a huge value
  MinuteAgg m3;
  m3.add(reading(5, 100));
  Rec r4 = m3.finish(1700000100, &r, 635200, false);
  CHECK_EQ(r4.imp_Wh, 0);
  // Counter jumps forward by a lot (demo -> real meter): not counted
  MinuteAgg m4;
  m4.add(reading(9000000, 100));
  m4.add(reading(9000003, 100));
  Rec r5 = m4.finish(1700000100, &r, 635200, false);
  CHECK_EQ(r5.imp_Wh, 3);
  // Demo record followed by a real one: never contiguous
  Rec demo_prev = r;
  demo_prev.flags |= REC_DEMO;
  Rec r6 = m2.finish(1700000100, &demo_prev, 635200, false);
  CHECK_EQ(r6.imp_Wh, 10);
}

static void testRecAggImplausibleGap() {
  RecAgg a(true);
  MinuteAgg m;
  m.add(reading(1000, 600));
  m.add(reading(1010, 600));
  Rec r1 = m.finish(1700000040, nullptr, 600000, false);
  a.add(r1);
  MinuteAgg m2;  // next minute, counter jumped by 500 kWh: ignored
  m2.add(reading(501010, 600));
  m2.add(reading(501020, 600));
  Rec r2 = m2.finish(1700000100, &r1, 600000, false);
  a.add(r2);
  Rec h = a.finish(1700000040, 600000);
  CHECK_EQ(h.imp_Wh, 20);
  CHECK_EQ(h.imp_end, 501020);
}

static void testRecAgg() {
  RecAgg hour(true);
  Rec prev;
  memset(&prev, 0, sizeof(prev));
  uint32_t counter = 2000000;
  const uint32_t t0 = 1700000000 - 1700000000 % 3600;
  bool have_prev = false;
  for (int i = 0; i < 60; i++) {
    if (i >= 20 && i < 25) { counter += 50; continue; }  // 5 minute reboot gap: 250 Wh
    MinuteAgg m;
    uint32_t p = (i == 40) ? 3000 : 600;
    m.add(reading(counter, p));
    counter += 10;
    m.add(reading(counter, p));
    Rec r = m.finish(t0 + i * 60, have_prev ? &prev : nullptr, 600000, false);
    hour.add(r);
    prev = r;
    have_prev = true;
  }
  Rec h = hour.finish(t0, 600000);
  CHECK_EQ(hour.count(), 55);
  CHECK_EQ(h.samples, 55);
  CHECK_EQ(h.imp_end, counter);
  // whole hour from the counters, gap included
  CHECK_EQ(h.imp_Wh, counter - 2000000);
  CHECK_EQ(h.p_max, 3000);
  CHECK_EQ(hour.peakTs(), t0 + 40 * 60);
  CHECK_EQ(h.p_min, 600);
  CHECK_EQ(h.cost_u, costMicro(h.imp_Wh, 600000));

  // Day from hours: p_min takes the hours' p_min
  RecAgg day(false);
  Rec h2 = h;
  h2.ts = t0 + 3600;
  h2.p_min = 250;
  h2.imp_end = h.imp_end + 400;
  h2.imp_Wh = 400;
  h2.cost_u = costMicro(400, 600000);
  day.add(h);
  day.add(h2);
  Rec d = day.finish(t0, 600000);
  CHECK_EQ(d.p_min, 250);
  CHECK_EQ(d.imp_Wh, h.imp_Wh + 400);
  CHECK_EQ(d.samples, 2);
}

static void testTariff() {
  TariffSettings t;
  t.price_u = 600000;
  CHECK_EQ(priceAt(t, 0, 12), 600000);
  // Night 23 -> 7, every day, 20%
  t.rules[0].enabled = 1; t.rules[0].start_hour = 23; t.rules[0].end_hour = 7; t.rules[0].pct_x10 = 200;
  // Day 7 -> 17, Sunday..Thursday, 15%
  t.rules[1].enabled = 1; t.rules[1].days = 0x1F; t.rules[1].start_hour = 7; t.rules[1].end_hour = 17; t.rules[1].pct_x10 = 150;
  CHECK_EQ(discountAt(t, 3, 23), 200);
  CHECK_EQ(discountAt(t, 3, 2), 200);
  CHECK_EQ(discountAt(t, 3, 7), 150);
  CHECK_EQ(discountAt(t, 3, 16), 150);
  CHECK_EQ(discountAt(t, 3, 17), 0);
  CHECK_EQ(discountAt(t, 5, 10), 0);   // Friday: day plan not valid
  CHECK_EQ(priceAt(t, 3, 2), 480000);
  // Flat discount all day
  TariffSettings f;
  f.price_u = 600000;
  f.rules[0].enabled = 1; f.rules[0].start_hour = 0; f.rules[0].end_hour = 24; f.rules[0].pct_x10 = 70;
  for (int h = 0; h < 24; h++) CHECK_EQ(discountAt(f, 6, h), 70);
  // Disabled rule is ignored
  f.rules[0].enabled = 0;
  CHECK_EQ(discountAt(f, 6, 3), 0);
}

int main() {
  testFixed();
  testSinglePhase();
  testBadCrcAndRecovery();
  testDemoTelegram();
  testMinuteAgg();
  testRecAgg();
  testRecAggImplausibleGap();
  testTariff();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
