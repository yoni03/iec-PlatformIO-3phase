// Login: salted, iterated SHA-256 password hash; random session tokens in a
// cookie; lockout after repeated failures.
#pragma once
#include <Arduino.h>

#include "settings.h"

namespace auth {
void setPassword(Settings &s, const String &user, const String &password);
bool checkPassword(const Settings &s, const String &user, const String &password);

// Returns a new session token ("" if locked out). remember = 30 days instead of 12 hours.
String login(const Settings &s, const String &user, const String &password, bool remember,
             uint32_t *max_age_s, uint32_t *locked_for_ms);
bool valid(const String &token);
void logout(const String &token);
void logoutAll();
// Reads the "sid" value out of a Cookie header
String tokenFromCookie(const String &cookie);
}  // namespace auth
