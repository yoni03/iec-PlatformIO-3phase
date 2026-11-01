#include "auth.h"

#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

// mbedTLS 3 (Arduino-ESP32 3.x) renamed the *_ret functions
#if MBEDTLS_VERSION_MAJOR >= 3
#define SHA_START(c) mbedtls_sha256_starts(c, 0)
#define SHA_UPDATE(c, d, n) mbedtls_sha256_update(c, d, n)
#define SHA_FINISH(c, o) mbedtls_sha256_finish(c, o)
#else
#define SHA_START(c) mbedtls_sha256_starts_ret(c, 0)
#define SHA_UPDATE(c, d, n) mbedtls_sha256_update_ret(c, d, n)
#define SHA_FINISH(c, o) mbedtls_sha256_finish_ret(c, o)
#endif

#include "config.h"

namespace {

struct Session {
  char token[33] = {0};
  int64_t expires_ms = 0;
};
Session sessions[MAX_SESSIONS];
int fails = 0;
int64_t locked_until_ms = 0;

int64_t nowMs() { return esp_timer_get_time() / 1000; }

void hash(const uint8_t salt[16], const String &password, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  SHA_START(&ctx);
  SHA_UPDATE(&ctx, salt, 16);
  SHA_UPDATE(&ctx, (const uint8_t *)password.c_str(), password.length());
  SHA_FINISH(&ctx, out);
  // Key stretching: makes brute force on a stolen hash slower (~20 ms on the ESP32)
  for (int i = 0; i < 2000; i++) {
    SHA_START(&ctx);
    SHA_UPDATE(&ctx, out, 32);
    SHA_UPDATE(&ctx, salt, 16);
    SHA_FINISH(&ctx, out);
  }
  mbedtls_sha256_free(&ctx);
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

}  // namespace

namespace auth {

void setPassword(Settings &s, const String &user, const String &password) {
  esp_fill_random(s.admin_salt, sizeof(s.admin_salt));
  hash(s.admin_salt, password, s.admin_hash);
  s.admin_user = user;
  s.admin_set = true;
}

bool checkPassword(const Settings &s, const String &user, const String &password) {
  if (!s.admin_set) return false;
  uint8_t h[32];
  hash(s.admin_salt, password, h);
  const bool user_ok = user.equalsIgnoreCase(s.admin_user);
  return constantTimeEqual(h, s.admin_hash, 32) && user_ok;
}

String login(const Settings &s, const String &user, const String &password, bool remember,
             uint32_t *max_age_s, uint32_t *locked_for_ms) {
  const int64_t now = nowMs();
  *locked_for_ms = 0;
  if (now < locked_until_ms) {
    *locked_for_ms = (uint32_t)(locked_until_ms - now);
    return "";
  }
  if (!checkPassword(s, user, password)) {
    if (++fails >= LOGIN_MAX_FAILS) {
      fails = 0;
      locked_until_ms = now + LOGIN_LOCKOUT_MS;
      *locked_for_ms = LOGIN_LOCKOUT_MS;
    }
    return "";
  }
  fails = 0;
  // Reuse an expired slot, or replace the one that expires first
  Session *slot = &sessions[0];
  for (Session &ss : sessions) {
    if (ss.expires_ms < slot->expires_ms) slot = &ss;
  }
  uint8_t rnd[16];
  esp_fill_random(rnd, sizeof(rnd));
  for (int i = 0; i < 16; i++) sprintf(slot->token + i * 2, "%02x", rnd[i]);
  const uint32_t hours = remember ? SESSION_HOURS_LONG : SESSION_HOURS_SHORT;
  slot->expires_ms = now + (int64_t)hours * 3600 * 1000;
  *max_age_s = hours * 3600;
  return String(slot->token);
}

bool valid(const String &token) {
  if (token.length() != 32) return false;
  const int64_t now = nowMs();
  for (Session &ss : sessions) {
    if (ss.expires_ms > now && constantTimeEqual((const uint8_t *)ss.token,
                                                 (const uint8_t *)token.c_str(), 32))
      return true;
  }
  return false;
}

void logout(const String &token) {
  for (Session &ss : sessions) {
    if (token.length() == 32 && token.equals(ss.token)) ss = Session();
  }
}

void logoutAll() {
  for (Session &ss : sessions) ss = Session();
}

String tokenFromCookie(const String &cookie) {
  int i = cookie.indexOf("sid=");
  while (i > 0 && cookie[i - 1] != ' ' && cookie[i - 1] != ';') i = cookie.indexOf("sid=", i + 1);
  if (i < 0) return "";
  int end = cookie.indexOf(';', i);
  return cookie.substring(i + 4, end < 0 ? cookie.length() : end);
}

}  // namespace auth
