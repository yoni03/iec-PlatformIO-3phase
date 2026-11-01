// Wi-Fi: home network (station) with a setup / fallback access point and
// a captive portal, plus mDNS (http://<hostname>.local)
#pragma once
#include <Arduino.h>

#include "settings.h"

namespace net {
void begin(const Settings &s);
void loop();
bool connected();       // to the home Wi-Fi
bool apActive();        // the setup / fallback access point (AP_SSID) is on
bool setupMode();       // first-time setup: no Wi-Fi configured yet
String ip();
int rssi();
// Wi-Fi scan for the settings page. Returns "" while still scanning.
String scanJson();
}  // namespace net
