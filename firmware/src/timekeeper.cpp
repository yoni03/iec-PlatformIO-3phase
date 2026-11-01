#include "timekeeper.h"

#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"

namespace {
volatile bool ntp_synced = false;
bool from_meter = false;
uint32_t last_meter_set_ms = 0;

void onNtpSync(struct timeval *) {
  ntp_synced = true;
  from_meter = false;
}
}  // namespace

namespace timekeeper {

void begin() {
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
}

bool valid() { return time(nullptr) > 1700000000; }

const char *source() {
  if (!valid()) return "none";
  return from_meter && !ntp_synced ? "meter" : "ntp";
}

void meterTime(const char *s) {
  if (ntp_synced) return;
  // Wait a little for NTP first; then take the meter's clock and refresh it hourly
  if (millis() < METER_TIME_FALLBACK_MS) return;
  if (from_meter && millis() - last_meter_set_ms < 3600UL * 1000) return;
  if (strlen(s) < 12) return;
  auto two = [&](int i) { return (s[i] - '0') * 10 + (s[i + 1] - '0'); };
  for (int i = 0; i < 12; i++)
    if (s[i] < '0' || s[i] > '9') return;
  struct tm lt = {};
  lt.tm_year = 2000 + two(0) - 1900;
  lt.tm_mon = two(2) - 1;
  lt.tm_mday = two(4);
  lt.tm_hour = two(6);
  lt.tm_min = two(8);
  lt.tm_sec = two(10);
  lt.tm_isdst = s[12] == 'S' ? 1 : s[12] == 'W' ? 0 : -1;
  const time_t t = mktime(&lt);  // TZ_INFO is set, so this is local -> UTC
  if (t < 1700000000) return;
  struct timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
  from_meter = true;
  last_meter_set_ms = millis();
  Serial.printf("[time] clock set from the meter: %s\n", s);
}

uint32_t dayStart(uint32_t ts) {
  struct tm lt;
  local(ts, &lt);
  lt.tm_hour = 0;
  lt.tm_min = 0;
  lt.tm_sec = 0;
  lt.tm_isdst = -1;
  return (uint32_t)mktime(&lt);
}

void local(uint32_t ts, struct tm *out) {
  time_t t = ts;
  localtime_r(&t, out);
}

}  // namespace timekeeper
