// Settings kept in NVS (survive reboots and firmware updates)
#pragma once
#include <Arduino.h>

#include "tariff.h"

struct Settings {
  String wifi_ssid;
  String wifi_pass;
  String hostname;
  String admin_user;
  uint8_t admin_salt[16] = {0};
  uint8_t admin_hash[32] = {0};
  bool admin_set = false;
  TariffSettings tariff;
  uint8_t breaker_a = 25;
  bool demo = false;

  bool configured() const { return admin_set && wifi_ssid.length() > 0; }
};

namespace settings {
void load(Settings &s);
void save(const Settings &s);
void factoryReset();  // forgets Wi-Fi and login, keeps price settings and history
}  // namespace settings
