// Compile-time configuration. Most things (Wi-Fi, login, price, discounts)
// are set from the web UI and do not need a rebuild.
#pragma once

#define FW_VERSION "1.0.0"

// ---- P1 port --------------------------------------------------------------
#define P1_RX_PIN 16          // RJ12 pin 5 (Data), pulled up to 3V3 by 1k
#define P1_BAUD 115200
#define P1_INVERTED true      // the P1 data line is open-collector, inverted
#define P1_RX_BUFFER 8192     // ~8 telegrams of slack while the web server is busy

// ---- Board ----------------------------------------------------------------
#define STATUS_LED_PIN 2      // on-board LED of most ESP32 DevKit boards, -1 to disable
#define RESET_BUTTON_PIN 0    // BOOT button. Hold 10s = forget Wi-Fi and login
#define RESET_HOLD_MS 10000

// ---- Network --------------------------------------------------------------
#define DEFAULT_HOSTNAME "electricity-meter"   // http://electricity-meter.local
#define AP_SSID "HElectricity"                 // setup / fallback access point
#define AP_PASSWORD "meter1234"                // min 8 chars
#define WIFI_FALLBACK_AFTER_MS (3UL * 60 * 1000) // open the AP if Wi-Fi is down this long

// ---- Optional: home Wi-Fi (and login) set before flashing ----------------
// Copy secrets.example.h to secrets.h and fill it in. secrets.h is in
// .gitignore, so it is never pushed to GitHub. Without it, the device starts
// the setup access point (AP_SSID) and is configured from the browser.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef PRESET_WIFI_SSID
#define PRESET_WIFI_SSID ""
#endif
#ifndef PRESET_WIFI_PASSWORD
#define PRESET_WIFI_PASSWORD ""
#endif
#ifndef PRESET_HOSTNAME
#define PRESET_HOSTNAME ""
#endif
#ifndef PRESET_ADMIN_USER
#define PRESET_ADMIN_USER "admin"
#endif
#ifndef PRESET_ADMIN_PASSWORD
#define PRESET_ADMIN_PASSWORD ""
#endif

// ---- Time -----------------------------------------------------------------
// Israel: DST from the Friday before the last Sunday of March 02:00,
// until the last Sunday of October 02:00.
#define TZ_INFO "IST-2IDT,M3.4.4/26,M10.5.0"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
// If there is no internet, the clock is taken from the meter after this delay
#define METER_TIME_FALLBACK_MS (90UL * 1000)

// ---- History (LittleFS, 1.375MB partition) --------------------------------
// One record = 52 bytes. Files are append-only, one per period.
//   minute files: 1 per day,   1440 rec = 75KB  -> keep 5  (=375KB, today + 4 days)
//   hour files:   1 per month,  744 rec = 39KB  -> keep 13 (=505KB, ~1 year)
//   day files:    1 per year,   366 rec = 19KB  -> keep 6  (=114KB, ~5 years)
// Total ~1.0MB worst case (73% of the partition). Oldest minute files are
// also pruned automatically if the file system passes FS_PRUNE_PERCENT.
#define KEEP_MINUTE_FILES 5
#define KEEP_HOUR_FILES 13
#define KEEP_DAY_FILES 6
#define FS_PRUNE_PERCENT 85
#define MINUTE_FLUSH_EVERY 5  // write minute records in batches (less flash wear)

// ---- Live view ------------------------------------------------------------
#define LIVE_BUFFER_SAMPLES 900  // ~15 minutes at one telegram per second

// ---- Login ----------------------------------------------------------------
#define MAX_SESSIONS 6
#define SESSION_HOURS_SHORT 12
#define SESSION_HOURS_LONG (24 * 30)  // "remember me"
#define LOGIN_MAX_FAILS 5
#define LOGIN_LOCKOUT_MS 60000

// ---- Defaults for the cost settings (editable in the UI) ------------------
#define DEFAULT_PRICE_UILS 635200  // 0.6352 ILS/kWh incl. VAT (IEC, from 1.7.2026)
#define DEFAULT_BREAKER_A 25       // per phase, for the load bars
