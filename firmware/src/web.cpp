#include "web.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <sys/time.h>

#include "auth.h"
#include "config.h"
#include "meter.h"
#include "net.h"
#include "storage.h"
#include "timekeeper.h"
#include "web_ui.h"

namespace {

WebServer server(80);
Settings *cfg = nullptr;
uint32_t reboot_at = 0;
bool update_authorized = false;
String update_error;

// ---------------------------------------------------------------- helpers

String esc(const String &s) {
  String o;
  o.reserve(s.length() + 4);
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendError(int code, const char *error) {
  sendJson(code, String("{\"error\":\"") + error + "\"}");
}

void requestReboot(uint32_t delay_ms = 1500) { reboot_at = millis() + delay_ms | 1; }

bool isAuthed() { return auth::valid(auth::tokenFromCookie(server.header("Cookie"))); }

// Every API call except status/login/setup needs a session. State-changing
// calls must also carry X-Requested-With, which a cross-site form cannot set.
bool guard(bool post) {
  if (!isAuthed()) {
    sendError(401, "auth");
    return false;
  }
  if (post && server.header("X-Requested-With") != "fetch") {
    sendError(403, "csrf");
    return false;
  }
  return true;
}

uint32_t argU(const char *name, uint32_t def) {
  return server.hasArg(name) ? (uint32_t)strtoul(server.arg(name).c_str(), nullptr, 10) : def;
}

uint64_t nowEpochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return timekeeper::valid() ? (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}

// ---------------------------------------------------------------- pages

void handleIndex() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                    "img-src 'self' data:");
  server.send_P(200, "text/html; charset=utf-8", (const char *)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void handleNotFound() {
  // Captive portal: phones probe random URLs when they join the access point
  if (net::apActive() && !server.uri().startsWith("/api/")) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

// ---------------------------------------------------------------- session

void handleStatus() {
  String j = "{\"configured\":";
  j += cfg->configured() ? "true" : "false";
  j += ",\"authed\":";
  j += isAuthed() ? "true" : "false";
  j += ",\"setup\":";
  j += net::setupMode() ? "true" : "false";
  j += ",\"has_wifi\":";
  j += cfg->wifi_ssid.length() ? "true" : "false";
  j += ",\"has_user\":";
  j += cfg->admin_set ? "true" : "false";
  j += ",\"ap\":";
  j += net::apActive() ? "true" : "false";
  j += ",\"host\":\"" + esc(cfg->hostname) + "\",\"ver\":\"" FW_VERSION "\",\"ap_ssid\":\"" AP_SSID "\"}";
  sendJson(200, j);
}

void handleLogin() {
  if (server.header("X-Requested-With") != "fetch") return sendError(403, "csrf");
  uint32_t max_age = 0, locked = 0;
  const String token = auth::login(*cfg, server.arg("user"), server.arg("pass"),
                                   server.arg("remember") == "1", &max_age, &locked);
  if (locked) return sendJson(429, String("{\"error\":\"locked\",\"wait\":") + locked + "}");
  if (token.length() == 0) return sendError(401, "bad");
  server.sendHeader("Set-Cookie", "sid=" + token + "; Path=/; Max-Age=" + String(max_age) +
                                      "; HttpOnly; SameSite=Strict");
  sendJson(200, "{\"ok\":true}");
}

void handleLogout() {
  auth::logout(auth::tokenFromCookie(server.header("Cookie")));
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  sendJson(200, "{\"ok\":true}");
}

// First-time setup, only while nothing is configured. If the home Wi-Fi was
// preset in secrets.h, the Wi-Fi fields may be left empty: only the user is created.
void handleSetup() {
  if (cfg->configured() && !isAuthed()) return sendError(401, "auth");
  if (server.header("X-Requested-With") != "fetch") return sendError(403, "csrf");
  const String ssid = server.arg("ssid"), user = server.arg("user"), pass = server.arg("pass");
  const bool wifi_change = ssid.length() > 0;
  if (!wifi_change && cfg->wifi_ssid.length() == 0) return sendError(400, "ssid");
  if (ssid.length() > 32) return sendError(400, "ssid");
  if (user.length() == 0 || user.length() > 32) return sendError(400, "user");
  if (pass.length() < 8 || pass.length() > 64) return sendError(400, "pass");
  if (wifi_change) {
    cfg->wifi_ssid = ssid;
    cfg->wifi_pass = server.arg("wpass");
  }
  String host = server.arg("host");
  host.trim();
  host.toLowerCase();
  const bool host_change = host.length() && host != cfg->hostname;
  if (host.length()) cfg->hostname = host;
  auth::setPassword(*cfg, user, pass);
  settings::save(*cfg);
  const bool reboot = wifi_change || host_change || net::setupMode();
  sendJson(200, "{\"ok\":true,\"reboot\":" + String(reboot ? "true" : "false") + ",\"host\":\"" +
                    esc(cfg->hostname) + "\"}");
  if (reboot) requestReboot(2500);
}

// ---------------------------------------------------------------- live data

void handleLive() {
  if (!guard(false)) return;
  const MeterReading &m = meter::last();
  const TodaySummary t = meter::today();
  const P1Stats &st = meter::stats();
  char buf[1400];
  const uint64_t now_ms = nowEpochMs();
  int n = snprintf(
      buf, sizeof(buf),
      "{\"up\":%lu,\"now\":%llu,\"tsrc\":\"%s\",\"ok\":%s,\"age\":%lu,"
      "\"m\":{\"p\":%lu,\"pe\":%lu,\"ph\":%d,\"pp\":%s,\"tariff\":%u,\"imp\":%lu,\"exp\":%lu,"
      "\"t\":[%lu,%lu,%lu],\"l\":[[%lu,%u,%u],[%lu,%u,%u],[%lu,%u,%u]]},"
      "\"today\":{\"ok\":%s,\"day\":%lu,\"imp\":%lu,\"exp\":%lu,\"cost\":%lu,\"peak\":%lu,"
      "\"peak_ts\":%lu,\"base\":%lu},"
      "\"price\":%lu,\"disc\":%u,"
      "\"st\":{\"ok\":%lu,\"crc\":%lu,\"ovf\":%lu},"
      "\"sys\":{\"heap\":%lu,\"rssi\":%d,\"wifi\":%s,\"ap\":%s,\"fs_used\":%u,\"fs_total\":%u,"
      "\"demo\":%s}}",
      (unsigned long)millis(), (unsigned long long)now_ms, timekeeper::source(),
      meter::haveReading() ? "true" : "false",
      (unsigned long)(meter::haveReading() ? meter::readingAgeMs() : 0),
      (unsigned long)m.p_imp_W, (unsigned long)m.p_exp_W, m.phases(),
      (m.present & HAS_PL1) ? "true" : "false", m.tariff, (unsigned long)m.importTotalWh(),
      (unsigned long)m.exportTotalWh(), (unsigned long)m.imp_Wh[0], (unsigned long)m.imp_Wh[1],
      (unsigned long)m.imp_Wh[2],
      (unsigned long)((m.present & HAS_PL1) ? m.pl_W[0] : m.p_imp_W), m.v_dV[0], m.i_cA[0],
      (unsigned long)m.pl_W[1], m.v_dV[1], m.i_cA[1], (unsigned long)m.pl_W[2], m.v_dV[2],
      m.i_cA[2], t.valid ? "true" : "false", (unsigned long)t.day_ts, (unsigned long)t.imp_Wh,
      (unsigned long)t.exp_Wh, (unsigned long)t.cost_u, (unsigned long)t.peak_W,
      (unsigned long)t.peak_ts, (unsigned long)t.base_W, (unsigned long)meter::currentPriceU(),
      meter::currentDiscountX10(), (unsigned long)st.ok, (unsigned long)st.crc_errors,
      (unsigned long)st.overflows, (unsigned long)ESP.getFreeHeap(), net::rssi(),
      net::connected() ? "true" : "false", net::apActive() ? "true" : "false",
      (unsigned)storage::usedBytes(), (unsigned)storage::totalBytes(),
      cfg->demo ? "true" : "false");
  if (n <= 0 || n >= (int)sizeof(buf)) return sendError(500, "overflow");
  sendJson(200, buf);
}

void handleRecent() {
  if (!guard(false)) return;
  static LiveSample samples[LIVE_BUFFER_SAMPLES];
  const size_t count = meter::liveSince(argU("since", 0), samples, LIVE_BUFFER_SAMPLES);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "");
  char buf[1024];
  int n = snprintf(buf, sizeof(buf), "{\"up\":%lu,\"now\":%llu,\"s\":[", (unsigned long)millis(),
                   (unsigned long long)nowEpochMs());
  for (size_t i = 0; i < count; i++) {
    const LiveSample &s = samples[i];
    n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,%u,%u,%u,%u,%u]", i ? "," : "",
                  (unsigned long)s.t, s.p, s.pe, s.l[0], s.l[1], s.l[2]);
    if (n > (int)sizeof(buf) - 64) {
      server.sendContent(buf, n);
      n = 0;
    }
  }
  n += snprintf(buf + n, sizeof(buf) - n, "]}");
  server.sendContent(buf, n);
  server.sendContent("");
}

// ---------------------------------------------------------------- history

void handleHistory() {
  if (!guard(false)) return;
  const Tier tier = storage::tierFromChar(server.arg("tier").c_str()[0]);
  const uint32_t from = argU("from", 0), to = argU("to", UINT32_MAX);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  // 8 byte header: magic, version, record size
  uint8_t buf[8 + 20 * sizeof(Rec)];
  memcpy(buf, "IECH", 4);
  buf[4] = 1; buf[5] = 0;
  buf[6] = sizeof(Rec) & 0xFF; buf[7] = sizeof(Rec) >> 8;
  size_t n = 8;
  storage::forEach(
      tier, from, to,
      [&](const Rec &r) {
        memcpy(buf + n, &r, sizeof(Rec));
        n += sizeof(Rec);
        if (n + sizeof(Rec) > sizeof(buf)) {
          server.sendContent((const char *)buf, n);
          n = 0;
          meter::service();
        }
        return true;
      },
      [] { meter::service(); });
  if (n) server.sendContent((const char *)buf, n);
  server.sendContent("");
}

void handleCsv() {
  if (!guard(false)) return;
  const char tc = server.arg("tier").c_str()[0];
  const Tier tier = storage::tierFromChar(tc);
  const uint32_t from = argU("from", 0), to = argU("to", UINT32_MAX);
  struct tm lt;
  timekeeper::local(from ? from : time(nullptr), &lt);
  char name[64];
  strftime(name, sizeof(name), "electricity-%Y%m%d", &lt);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + name + "-" +
                                               (tier == TIER_MINUTE ? "minutes" : tier == TIER_HOUR ? "hours" : "days") + ".csv\"");
  server.send(200, "text/csv; charset=utf-8", "");
  String head =
      "\xEF\xBB\xBF"  // UTF-8 BOM so Excel shows Hebrew correctly
      "זמן,צריכה (kWh),החזרה לרשת (kWh),עלות (₪),הספק ממוצע (kW),הספק מרבי (kW),"
      "הספק מזערי (kW),L1 ממוצע (kW),L2 ממוצע (kW),L3 ממוצע (kW),מתח L1 (V),מתח L2 (V),"
      "מתח L3 (V),זרם מרבי L1 (A),זרם מרבי L2 (A),זרם מרבי L3 (A),מונה צריכה (kWh),"
      "מונה החזרה (kWh),דגימות,מצב הדגמה\r\n";
  server.sendContent(head);
  char buf[1400];
  size_t n = 0;
  storage::forEach(
      tier, from, to,
      [&](const Rec &r) {
        struct tm t;
        timekeeper::local(r.ts, &t);
        char when[24];
        strftime(when, sizeof(when), tier == TIER_DAY ? "%Y-%m-%d" : "%Y-%m-%d %H:%M", &t);
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.2f,%.2f,"
                      "%.2f,%.3f,%.3f,%u,%s\r\n",
                      when, r.imp_Wh / 1000.0, r.exp_Wh / 1000.0, r.cost_u / 1e6, r.p_avg / 1000.0,
                      r.p_max / 1000.0, r.p_min / 1000.0, r.pl_avg[0] / 1000.0,
                      r.pl_avg[1] / 1000.0, r.pl_avg[2] / 1000.0, r.v_avg[0] / 10.0,
                      r.v_avg[1] / 10.0, r.v_avg[2] / 10.0, r.i_max[0] / 100.0, r.i_max[1] / 100.0,
                      r.i_max[2] / 100.0, r.imp_end / 1000.0, r.exp_end / 1000.0, r.samples,
                      (r.flags & REC_DEMO) ? "כן" : "");
        if (n > sizeof(buf) - 300) {
          server.sendContent(buf, n);
          n = 0;
          meter::service();
        }
        return true;
      },
      [] { meter::service(); });
  if (n) server.sendContent(buf, n);
  server.sendContent("");
}

// ---------------------------------------------------------------- settings

void handleGetSettings() {
  if (!guard(false)) return;
  String j = "{\"price\":" + String(cfg->tariff.price_u) + ",\"fixed\":" + String(cfg->tariff.fixed_ag) +
             ",\"bill_day\":" + String(cfg->tariff.bill_day) + ",\"breaker\":" + String(cfg->breaker_a) +
             ",\"demo\":" + (cfg->demo ? "true" : "false") + ",\"host\":\"" + esc(cfg->hostname) +
             "\",\"ssid\":\"" + esc(cfg->wifi_ssid) + "\",\"user\":\"" + esc(cfg->admin_user) +
             "\",\"ap_ssid\":\"" AP_SSID "\",\"ver\":\"" FW_VERSION "\",\"rules\":[";
  for (int i = 0; i < MAX_DISCOUNT_RULES; i++) {
    const DiscountRule &r = cfg->tariff.rules[i];
    if (i) j += ',';
    j += "{\"en\":" + String(r.enabled) + ",\"days\":" + String(r.days) + ",\"s\":" + String(r.start_hour) +
         ",\"e\":" + String(r.end_hour) + ",\"pct\":" + String(r.pct_x10) + "}";
  }
  j += "],\"fs\":{\"m\":" + String(storage::fileCount(TIER_MINUTE)) + ",\"h\":" +
       String(storage::fileCount(TIER_HOUR)) + ",\"d\":" + String(storage::fileCount(TIER_DAY)) + "}}";
  sendJson(200, j);
}

void handleSaveSettings() {
  if (!guard(true)) return;
  TariffSettings &t = cfg->tariff;
  const uint32_t price = argU("price", t.price_u);
  if (price < 10000 || price > 10000000) return sendError(400, "price");  // 0.01..10 ILS
  t.price_u = price;
  t.fixed_ag = min<uint32_t>(argU("fixed", t.fixed_ag), 100000);
  t.bill_day = constrain(argU("bill_day", t.bill_day), 1u, 28u);
  cfg->breaker_a = constrain(argU("breaker", cfg->breaker_a), 6u, 100u);
  if (server.hasArg("demo")) cfg->demo = server.arg("demo") == "1";
  for (int i = 0; i < MAX_DISCOUNT_RULES; i++) {
    const String p = "r" + String(i) + "_";
    if (!server.hasArg(p + "en")) continue;
    DiscountRule &r = t.rules[i];
    r.enabled = argU((p + "en").c_str(), 0) ? 1 : 0;
    r.days = argU((p + "days").c_str(), 0x7F) & 0x7F;
    r.start_hour = min<uint32_t>(argU((p + "s").c_str(), 0), 23);
    r.end_hour = constrain(argU((p + "e").c_str(), 24), 1u, 24u);
    r.pct_x10 = min<uint32_t>(argU((p + "pct").c_str(), 0), 1000);
  }
  settings::save(*cfg);
  sendJson(200, "{\"ok\":true}");
}

void handleWifi() {
  if (!guard(true)) return;
  const String ssid = server.arg("ssid");
  if (ssid.length() == 0 || ssid.length() > 32) return sendError(400, "ssid");
  // Keep the old password if the network did not change and the field is empty
  if (ssid != cfg->wifi_ssid || server.arg("wpass").length()) cfg->wifi_pass = server.arg("wpass");
  cfg->wifi_ssid = ssid;
  String host = server.arg("host");
  host.trim();
  host.toLowerCase();
  if (host.length()) cfg->hostname = host;
  settings::save(*cfg);
  sendJson(200, "{\"ok\":true}");
  requestReboot(2000);
}

void handleAccount() {
  if (!guard(true)) return;
  if (!auth::checkPassword(*cfg, cfg->admin_user, server.arg("cur"))) return sendError(403, "cur");
  const String user = server.arg("user").length() ? server.arg("user") : cfg->admin_user;
  const String pass = server.arg("pass");
  if (pass.length() < 8 || pass.length() > 64) return sendError(400, "pass");
  auth::setPassword(*cfg, user, pass);
  settings::save(*cfg);
  auth::logoutAll();
  sendJson(200, "{\"ok\":true}");
}

void handleScan() {
  if (net::setupMode() ? false : !guard(false)) return;
  const String r = net::scanJson();
  if (r.length() == 0) return sendJson(202, "{\"scanning\":true}");
  sendJson(200, "{\"list\":" + r + "}");
}

void handleReboot() {
  if (!guard(true)) return;
  sendJson(200, "{\"ok\":true}");
  requestReboot();
}

void handleClear() {
  if (!guard(true)) return;
  if (server.arg("confirm") != "yes") return sendError(400, "confirm");
  meter::clearHistory();
  sendJson(200, "{\"ok\":true}");
}

// ---------------------------------------------------------------- firmware update

void handleUpdateDone() {
  if (!update_authorized) return sendError(401, "auth");
  if (update_error.length() || Update.hasError()) {
    sendJson(500, "{\"error\":\"" + esc(update_error.length() ? update_error : String(Update.errorString())) + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
  requestReboot(1500);
}

void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    update_error = "";
    update_authorized = isAuthed() && server.header("X-Requested-With") == "fetch";
    if (!update_authorized) return;
    meter::flushNow();
    Serial.printf("[ota] receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) update_error = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!update_authorized || update_error.length()) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) update_error = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!update_authorized || update_error.length()) return;
    if (!Update.end(true)) update_error = Update.errorString();
    else Serial.printf("[ota] %u bytes OK\n", up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    update_error = "aborted";
  }
}

}  // namespace

namespace web {

void begin(Settings *s) {
  cfg = s;
  static const char *headers[] = {"Cookie", "X-Requested-With"};
  server.collectHeaders(headers, 2);

  server.on("/", HTTP_GET, handleIndex);
  server.on("/favicon.ico", HTTP_GET, [] { server.send(204); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/setup", HTTP_POST, handleSetup);
  server.on("/api/live", HTTP_GET, handleLive);
  server.on("/api/recent", HTTP_GET, handleRecent);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/export.csv", HTTP_GET, handleCsv);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handleSaveSettings);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/account", HTTP_POST, handleAccount);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/clear-history", HTTP_POST, handleClear);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[web] server on port 80");
}

void loop() { server.handleClient(); }

bool rebootRequested() { return reboot_at && (int32_t)(millis() - reboot_at) >= 0; }

}  // namespace web
