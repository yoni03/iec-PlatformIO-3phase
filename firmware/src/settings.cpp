#include "settings.h"

#include <Preferences.h>

#include "config.h"

namespace {
const char *kNs = "iecmeter";
}

namespace settings {

void load(Settings &s) {
  Preferences p;
  p.begin(kNs, true);
  s.wifi_ssid = p.getString("ssid", "");
  s.wifi_pass = p.getString("wpass", "");
  s.hostname = p.getString("host", DEFAULT_HOSTNAME);
  s.admin_user = p.getString("user", "");
  s.admin_set = p.getBytes("salt", s.admin_salt, sizeof(s.admin_salt)) == sizeof(s.admin_salt) &&
                p.getBytes("hash", s.admin_hash, sizeof(s.admin_hash)) == sizeof(s.admin_hash) &&
                s.admin_user.length() > 0;
  s.tariff.price_u = p.getUInt("price", DEFAULT_PRICE_UILS);
  s.tariff.fixed_ag = p.getUInt("fixed", 0);
  s.tariff.bill_day = p.getUChar("billday", 1);
  if (s.tariff.bill_day < 1 || s.tariff.bill_day > 28) s.tariff.bill_day = 1;
  if (p.getBytesLength("rules") == sizeof(s.tariff.rules)) {
    p.getBytes("rules", s.tariff.rules, sizeof(s.tariff.rules));
  }
  s.breaker_a = p.getUChar("breaker", DEFAULT_BREAKER_A);
  s.demo = p.getBool("demo", false);
  p.end();
  if (s.hostname.length() == 0) s.hostname = DEFAULT_HOSTNAME;
}

void save(const Settings &s) {
  Preferences p;
  p.begin(kNs, false);
  p.putString("ssid", s.wifi_ssid);
  p.putString("wpass", s.wifi_pass);
  p.putString("host", s.hostname);
  p.putString("user", s.admin_user);
  if (s.admin_set) {
    p.putBytes("salt", s.admin_salt, sizeof(s.admin_salt));
    p.putBytes("hash", s.admin_hash, sizeof(s.admin_hash));
  }
  p.putUInt("price", s.tariff.price_u);
  p.putUInt("fixed", s.tariff.fixed_ag);
  p.putUChar("billday", s.tariff.bill_day);
  p.putBytes("rules", s.tariff.rules, sizeof(s.tariff.rules));
  p.putUChar("breaker", s.breaker_a);
  p.putBool("demo", s.demo);
  p.end();
}

void factoryReset() {
  Preferences p;
  p.begin(kNs, false);
  p.remove("ssid");
  p.remove("wpass");
  p.remove("user");
  p.remove("salt");
  p.remove("hash");
  p.remove("host");
  p.end();
}

}  // namespace settings
