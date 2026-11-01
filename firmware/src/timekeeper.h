// Clock: NTP when there is internet, otherwise the meter's own clock
#pragma once
#include <Arduino.h>

namespace timekeeper {
void begin();
bool valid();
const char *source();  // "ntp", "meter" or "none"
// Called with 0-0:1.0.0 from every telegram (YYMMDDhhmmssX)
void meterTime(const char *yymmddhhmmssX);
// Local midnight of the day that contains ts
uint32_t dayStart(uint32_t ts);
void local(uint32_t ts, struct tm *out);
}  // namespace timekeeper
