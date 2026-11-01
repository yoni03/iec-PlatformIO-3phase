#include "meter.h"

#include <time.h>

#include "config.h"
#include "demo.h"
#include "storage.h"
#include "tariff.h"
#include "timekeeper.h"

namespace {

Settings *cfg = nullptr;
P1Parser parser;
DemoMeter demo;
uint32_t last_demo_ms = 0;

MeterReading last_reading;
bool have_reading = false;
uint32_t last_reading_ms = 0;

LiveSample live[LIVE_BUFFER_SAMPLES];
size_t live_head = 0, live_count = 0;

// History state. Aggregation is in RAM; flash writes are queued for tick().
bool restored = false;
MinuteAgg minute_agg;
uint32_t minute_ts = 0;
Rec prev_minute;
bool have_prev_minute = false;
RecAgg hour_agg(true), day_agg(true);
uint32_t hour_ts = 0, day_ts = 0;

const size_t kQueue = 64;
Rec q_minute[kQueue];
size_t q_minute_n = 0;
Rec q_hour[4], q_day[4];
size_t q_hour_n = 0, q_day_n = 0;
bool prune_due = true;

uint32_t priceFor(uint32_t ts) {
  struct tm lt;
  timekeeper::local(ts, &lt);
  return priceAt(cfg->tariff, lt.tm_wday, lt.tm_hour);
}

void queueHour(const Rec &r) {
  if (q_hour_n < 4) q_hour[q_hour_n++] = r;
}
void queueDay(const Rec &r) {
  if (q_day_n < 4) q_day[q_day_n++] = r;
}

void closeHour() {
  if (!hour_agg.empty()) queueHour(hour_agg.finish(hour_ts, priceFor(hour_ts)));
  hour_agg.clear();
}
void closeDay() {
  if (!day_agg.empty()) queueDay(day_agg.finish(day_ts, cfg->tariff.price_u));
  day_agg.clear();
  prune_due = true;
}

// One finished minute record -> queue + hour/day aggregates
void addMinute(const Rec &rec) {
  const uint32_t h = rec.ts - rec.ts % 3600;  // Israel's UTC offset is whole hours
  const uint32_t d = timekeeper::dayStart(rec.ts);
  if (hour_ts && h != hour_ts) {
    closeHour();
    if (have_prev_minute) hour_agg.seed(prev_minute);
  }
  if (day_ts && d != day_ts) {
    closeDay();
    if (have_prev_minute) day_agg.seed(prev_minute);
  }
  hour_ts = h;
  day_ts = d;
  hour_agg.add(rec);
  day_agg.add(rec);
  prev_minute = rec;
  have_prev_minute = true;
}

void closeMinute() {
  if (minute_agg.empty()) return;
  Rec rec = minute_agg.finish(minute_ts, have_prev_minute ? &prev_minute : nullptr,
                              priceFor(minute_ts), cfg->demo);
  minute_agg.clear();
  if (have_prev_minute && rec.ts <= prev_minute.ts) return;  // clock went backwards
  if (q_minute_n < kQueue) q_minute[q_minute_n++] = rec;
  addMinute(rec);
}

// After boot, rebuild today's and this hour's aggregates from the minute file,
// and write hour/day records that were missed while the device was off.
void restore() {
  const uint32_t now = time(nullptr);
  const uint32_t today = timekeeper::dayStart(now);
  const uint32_t this_hour = now - now % 3600;

  Rec last_min;
  if (storage::last(TIER_MINUTE, &last_min) && last_min.ts < now) {
    prev_minute = last_min;
    have_prev_minute = true;
    const uint32_t lh = last_min.ts - last_min.ts % 3600;
    const uint32_t ld = timekeeper::dayStart(last_min.ts);
    Rec last_h, last_d;
    const bool hh = storage::last(TIER_HOUR, &last_h), hd = storage::last(TIER_DAY, &last_d);
    // The hour of the last minute record ended without an hour record
    if (lh < this_hour && (!hh || last_h.ts < lh)) {
      RecAgg a(true);
      storage::forEach(TIER_MINUTE, lh, lh + 3600, [&](const Rec &r) { a.add(r); return true; });
      if (!a.empty()) {
        const Rec r = a.finish(lh, priceFor(lh));
        storage::append(TIER_HOUR, &r, 1);
      }
      Serial.println("[log] recovered a missed hour record");
    }
    if (ld < today && (!hd || last_d.ts < ld)) {
      RecAgg a(true);
      storage::forEach(TIER_MINUTE, ld, today, [&](const Rec &r) { a.add(r); return true; });
      if (!a.empty()) {
        const Rec r = a.finish(ld, cfg->tariff.price_u);
        storage::append(TIER_DAY, &r, 1);
      }
      Serial.println("[log] recovered a missed day record");
    }
  }
  // Today so far. The last minute before today / this hour seeds the
  // aggregates, so energy used while the device was off is not lost.
  day_agg.clear();
  hour_agg.clear();
  Rec before;
  bool have_before = false;
  storage::forEach(TIER_MINUTE, today - 3 * 3600, now + 60, [&](const Rec &r) {
    if (r.ts < today) {
      before = r;
      have_before = true;
      return true;
    }
    if (day_agg.empty() && have_before) day_agg.seed(before);
    day_agg.add(r);
    if (r.ts >= this_hour) {
      if (hour_agg.empty() && have_before) hour_agg.seed(before);
      hour_agg.add(r);
    } else {
      before = r;
      have_before = true;
    }
    return true;
  });
  day_ts = today;
  hour_ts = this_hour;
  Serial.printf("[log] restored %u minutes of today\n", (unsigned)day_agg.count());
}

void onTelegram(const MeterReading &r) {
  last_reading = r;
  have_reading = true;
  last_reading_ms = millis();
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, HIGH);
#endif

  LiveSample &s = live[live_head];
  s.t = last_reading_ms;
  s.p = r.p_imp_W > 65535 ? 65535 : r.p_imp_W;
  s.pe = r.p_exp_W > 65535 ? 65535 : r.p_exp_W;
  const bool pp = r.present & HAS_PL1;
  for (int k = 0; k < 3; k++) {
    uint32_t v = pp ? r.pl_W[k] : (k == 0 ? r.p_imp_W : 0);
    s.l[k] = v > 65535 ? 65535 : v;
  }
  live_head = (live_head + 1) % LIVE_BUFFER_SAMPLES;
  if (live_count < LIVE_BUFFER_SAMPLES) live_count++;

  if ((r.present & HAS_TIME) && !cfg->demo) timekeeper::meterTime(r.meter_time);
  if (!timekeeper::valid() || !restored) return;  // history needs a clock

  const uint32_t now = time(nullptr);
  const uint32_t m = now - now % 60;
  if (minute_ts && m != minute_ts) closeMinute();
  minute_ts = m;
  minute_agg.add(r);
}

}  // namespace

namespace meter {

void begin(Settings *s) {
  cfg = s;
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
#endif
  Serial2.setRxBufferSize(P1_RX_BUFFER);
  Serial2.begin(P1_BAUD, SERIAL_8N1, P1_RX_PIN, -1, P1_INVERTED);
}

void service() {
  if (cfg->demo) {
    while (Serial2.available()) Serial2.read();  // ignore a connected meter
    const uint32_t now_ms = millis();
    if (now_ms - last_demo_ms >= 1000) {
      const float dt = last_demo_ms ? (now_ms - last_demo_ms) / 1000.0f : 1.0f;
      last_demo_ms = now_ms;
      struct tm lt = {};
      time_t t = time(nullptr);
      bool clock = timekeeper::valid();
      if (clock) localtime_r(&t, &lt);
      static char buf[1200];
      const size_t n = demo.generate(buf, sizeof(buf), clock ? lt.tm_year + 1900 : 2026,
                                     clock ? lt.tm_mon + 1 : 1, clock ? lt.tm_mday : 1,
                                     clock ? lt.tm_hour : 12, clock ? lt.tm_min : 0,
                                     clock ? lt.tm_sec : 0, clock && lt.tm_isdst > 0, dt);
      for (size_t i = 0; i < n; i++)
        if (parser.feed((uint8_t)buf[i])) onTelegram(parser.reading());
    }
  } else {
    int budget = 4096;
    while (budget-- > 0 && Serial2.available()) {
      if (parser.feed((uint8_t)Serial2.read())) onTelegram(parser.reading());
    }
  }
#if STATUS_LED_PIN >= 0
  if (millis() - last_reading_ms > 40) digitalWrite(STATUS_LED_PIN, LOW);
#endif
}

void tick() {
  if (!restored && timekeeper::valid()) {
    restore();
    restored = true;
  }
  if (!restored) return;

  // A minute without telegrams still has to be closed (meter unplugged)
  const uint32_t now = time(nullptr);
  if (minute_ts && now - now % 60 != minute_ts && millis() - last_reading_ms > 2000) {
    closeMinute();
    minute_ts = 0;
  }
  // Hour/day ended with no data at all in the following minute
  if (hour_ts && now - now % 3600 != hour_ts && minute_agg.empty()) {
    closeHour();
    hour_ts = now - now % 3600;
  }
  if (day_ts && timekeeper::dayStart(now) != day_ts && minute_agg.empty()) {
    closeHour();
    closeDay();
    day_ts = timekeeper::dayStart(now);
  }

  if (q_minute_n >= MINUTE_FLUSH_EVERY || q_hour_n || q_day_n) flushNow();
  if (prune_due) {
    prune_due = false;
    storage::prune();
  }
}

void flushNow() {
  if (q_minute_n) storage::append(TIER_MINUTE, q_minute, q_minute_n);
  if (q_hour_n) storage::append(TIER_HOUR, q_hour, q_hour_n);
  if (q_day_n) storage::append(TIER_DAY, q_day, q_day_n);
  q_minute_n = q_hour_n = q_day_n = 0;
}

bool haveReading() { return have_reading; }
uint32_t readingAgeMs() { return have_reading ? millis() - last_reading_ms : UINT32_MAX; }
const MeterReading &last() { return last_reading; }
const P1Stats &stats() { return parser.stats(); }

uint32_t currentPriceU() { return timekeeper::valid() ? priceFor(time(nullptr)) : cfg->tariff.price_u; }

uint16_t currentDiscountX10() {
  if (!timekeeper::valid()) return 0;
  struct tm lt;
  timekeeper::local(time(nullptr), &lt);
  return discountAt(cfg->tariff, lt.tm_wday, lt.tm_hour);
}

TodaySummary today() {
  TodaySummary t;
  if (!restored) return t;
  t.valid = true;
  t.day_ts = day_ts ? day_ts : timekeeper::dayStart(time(nullptr));
  const Rec d = day_agg.finish(t.day_ts, cfg->tariff.price_u);
  t.imp_Wh = d.imp_Wh;
  t.exp_Wh = d.exp_Wh;
  t.cost_u = d.cost_u;
  t.peak_W = d.p_max;
  t.peak_ts = day_agg.peakTs();
  t.base_W = day_agg.empty() ? 0 : d.p_min;
  // Add the minute in progress
  if (!minute_agg.empty()) {
    const Rec m = minute_agg.finish(minute_ts, have_prev_minute ? &prev_minute : nullptr,
                                    currentPriceU(), false);
    t.imp_Wh += m.imp_Wh;
    t.exp_Wh += m.exp_Wh;
    t.cost_u += m.cost_u;
    if (m.p_max > t.peak_W) {
      t.peak_W = m.p_max;
      t.peak_ts = minute_ts;
    }
  }
  return t;
}

size_t liveSince(uint32_t since_ms, LiveSample *out, size_t max) {
  size_t n = 0;
  const size_t start = (live_head + LIVE_BUFFER_SAMPLES - live_count) % LIVE_BUFFER_SAMPLES;
  for (size_t k = 0; k < live_count && n < max; k++) {
    const LiveSample &s = live[(start + k) % LIVE_BUFFER_SAMPLES];
    if ((int32_t)(s.t - since_ms) > 0) out[n++] = s;
  }
  return n;
}

void clearHistory() {
  q_minute_n = q_hour_n = q_day_n = 0;
  storage::clearAll();
  minute_agg.clear();
  hour_agg.clear();
  day_agg.clear();
  have_prev_minute = false;
  minute_ts = 0;
}

}  // namespace meter
