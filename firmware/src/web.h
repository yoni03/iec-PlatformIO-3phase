// HTTP server: the web UI and its JSON / binary API
#pragma once
#include "settings.h"

namespace web {
void begin(Settings *s);
void loop();
bool rebootRequested();
}  // namespace web
