// The meter side of the application: reads the P1 port (or the demo
// generator), keeps the live buffer and builds the history.
#pragma once
#include <Arduino.h>

#include "p1_parser.h"
#include "records.h"
#include "settings.h"

struct LiveSample {
  uint32_t t;          // millis()
  uint16_t p, pe;      // total power drawn / returned (W)
  uint16_t l[3];       // per-phase power (W)
};

struct TodaySummary {
  bool valid = false;
  uint32_t day_ts = 0;      // local midnight
  uint32_t imp_Wh = 0, exp_Wh = 0;
  uint32_t cost_u = 0;
  uint32_t peak_W = 0, peak_ts = 0;
  uint32_t base_W = 0;      // lowest 1-minute average today
};

namespace meter {
void begin(Settings *s);
// Reads the serial port / demo generator. Cheap; safe to call from inside web
// handlers (it never touches the file system).
void service();
// Writes queued records to flash, handles day changes and pruning. loop() only.
void tick();
void flushNow();  // before a reboot

bool haveReading();
uint32_t readingAgeMs();
const MeterReading &last();
const P1Stats &stats();
TodaySummary today();
uint32_t currentPriceU();
uint16_t currentDiscountX10();

// Live samples newer than since_ms (millis), oldest first
size_t liveSince(uint32_t since_ms, LiveSample *out, size_t max);
void clearHistory();
}  // namespace meter
