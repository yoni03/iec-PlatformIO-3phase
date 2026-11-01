// Standalone P1 reader for Israeli IEC smart meters (Kaifa MA309M / MA110M).
// ESP32 + web UI with login, live data and on-device history.
#include <Arduino.h>
#include <time.h>

#include "auth.h"
#include "config.h"
#include "meter.h"
#include "net.h"
#include "settings.h"
#include "storage.h"
#include "timekeeper.h"
#include "web.h"

static Settings cfg;

static void checkResetButton() {
  static uint32_t pressed_since = 0;
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!pressed_since) pressed_since = millis() | 1;
    if (millis() - pressed_since > RESET_HOLD_MS) {
      Serial.println("[reset] BOOT held: forgetting Wi-Fi and login");
      settings::factoryReset();
      meter::flushNow();
      delay(200);
      ESP.restart();
    }
  } else {
    pressed_since = 0;
  }
}

static_assert(sizeof(PRESET_ADMIN_PASSWORD) == 1 || sizeof(PRESET_ADMIN_PASSWORD) >= 9,
              "secrets.h: PRESET_ADMIN_PASSWORD must have at least 8 characters");
static_assert(sizeof(PRESET_WIFI_SSID) <= 33, "secrets.h: PRESET_WIFI_SSID is longer than 32 characters");

// Values from secrets.h, used while the device has no settings of its own
static void applyPresets() {
  bool changed = false;
  if (cfg.wifi_ssid.length() == 0 && strlen(PRESET_WIFI_SSID) > 0) {
    cfg.wifi_ssid = PRESET_WIFI_SSID;
    cfg.wifi_pass = PRESET_WIFI_PASSWORD;
    if (strlen(PRESET_HOSTNAME) > 0) cfg.hostname = PRESET_HOSTNAME;
    changed = true;
    Serial.printf("[preset] home Wi-Fi from secrets.h: '%s'\n", PRESET_WIFI_SSID);
  }
  if (!cfg.admin_set && strlen(PRESET_ADMIN_PASSWORD) >= 8) {
    auth::setPassword(cfg, PRESET_ADMIN_USER, PRESET_ADMIN_PASSWORD);
    changed = true;
    Serial.printf("[preset] web login from secrets.h: user '%s'\n", PRESET_ADMIN_USER);
  }
  if (changed) settings::save(cfg);
}

static void printStatus() {
  static uint32_t last = 0;
  if (millis() - last < 60000) return;
  last = millis();
  const P1Stats &st = meter::stats();
  time_t now = time(nullptr);
  char when[32] = "no clock";
  if (timekeeper::valid()) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
  Serial.printf("[status] %s (%s) | P1 ok=%lu crc=%lu | power %lu W | wifi %s %s | heap %lu | fs %u%%\n",
                when, timekeeper::source(), (unsigned long)st.ok, (unsigned long)st.crc_errors,
                (unsigned long)meter::last().p_imp_W, net::connected() ? "up" : "down",
                net::ip().c_str(), (unsigned long)ESP.getFreeHeap(),
                (unsigned)(storage::usedBytes() * 100 / max<size_t>(storage::totalBytes(), 1)));
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n\nElectricity meter " FW_VERSION "\n");
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  settings::load(cfg);
  applyPresets();
  storage::begin();
  net::begin(cfg);
  timekeeper::begin();  // after Wi-Fi init (SNTP needs the network stack)
  meter::begin(&cfg);
  web::begin(&cfg);
  if (cfg.demo) Serial.println("[demo] demo mode is ON: the meter port is ignored");
}

void loop() {
  meter::service();
  web::loop();
  net::loop();
  meter::service();
  meter::tick();
  checkResetButton();
  printStatus();
  if (web::rebootRequested()) {
    meter::flushNow();
    delay(100);
    ESP.restart();
  }
  delay(2);
}
