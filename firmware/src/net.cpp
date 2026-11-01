#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"

namespace {
DNSServer dns;
bool setup_mode = false;
bool ap_on = false;
bool mdns_on = false;
String hostname;
uint32_t down_since = 0;      // millis() when the station lost Wi-Fi (0 = connected)
uint32_t up_since = 0;
uint32_t last_retry = 0;
bool scan_started = false;

void startAp() {
  if (ap_on) return;
  WiFi.mode(WIFI_AP_STA);  // STA stays on for scanning and reconnecting
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());  // captive portal: every name -> the ESP32
  ap_on = true;
  Serial.printf("[wifi] access point '%s' on, http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
}

void stopAp() {
  if (!ap_on) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap_on = false;
  Serial.println("[wifi] access point off");
}

String jsonEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}
}  // namespace

namespace net {

void begin(const Settings &s) {
  hostname = s.hostname;
  WiFi.persistent(false);  // credentials live in our own settings
  WiFi.setHostname(hostname.c_str());
  setup_mode = s.wifi_ssid.length() == 0;
  if (setup_mode) {
    Serial.println("[wifi] not configured: starting the setup access point");
    startAp();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Modem sleep stays on: the P1 port supplies limited current (DSMR: 250 mA)
  WiFi.begin(s.wifi_ssid.c_str(), s.wifi_pass.c_str());
  down_since = millis() | 1;
  Serial.printf("[wifi] connecting to '%s'\n", s.wifi_ssid.c_str());
}

void loop() {
  if (ap_on) dns.processNextRequest();
  if (setup_mode) return;

  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (down_since) {
      down_since = 0;
      up_since = now;
      Serial.printf("[wifi] connected, IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      if (!mdns_on && MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        mdns_on = true;
      }
    }
    // Back online for a minute and nobody on the access point: close it
    if (ap_on && now - up_since > 60000 && WiFi.softAPgetStationNum() == 0) stopAp();
  } else {
    if (!down_since) {
      down_since = now | 1;
      Serial.println("[wifi] connection lost");
    }
    if (!ap_on && now - down_since > WIFI_FALLBACK_AFTER_MS) startAp();
    // With the AP on, auto-reconnect is unreliable: retry every 30 s
    if (ap_on && now - last_retry > 30000) {
      last_retry = now;
      WiFi.reconnect();
    }
  }
}

bool connected() { return !setup_mode && WiFi.status() == WL_CONNECTED; }
bool apActive() { return ap_on; }
bool setupMode() { return setup_mode; }
String ip() { return connected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
int rssi() { return connected() ? WiFi.RSSI() : 0; }

String scanJson() {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "";
  if (n < 0 || !scan_started) {
    WiFi.scanNetworks(true);  // async
    scan_started = true;
    return "";
  }
  String out = "[";
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).length() == 0) continue;
    if (out.length() > 1) out += ',';
    out += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  out += "]";
  WiFi.scanDelete();
  scan_started = false;
  return out;
}

}  // namespace net
