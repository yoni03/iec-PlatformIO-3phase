// Home Wi-Fi and login, set before flashing (optional).
//
// 1. Copy this file to secrets.h (same folder).
// 2. Fill in your values.
// 3. Build and flash with PlatformIO.
//
// The device then joins your home network on the first boot, without the
// HElectricity setup network. secrets.h is listed in .gitignore and is never
// pushed to GitHub. Do not share the firmware.bin you build from it: the values
// below are inside it.
//
// The values are used only while the device has no settings of its own: on
// the first boot, and again after a reset with the BOOT button (10 s).
// Changes made later in the web UI take precedence.
#pragma once

// Home network (2.4 GHz only)
#define PRESET_WIFI_SSID "MyHomeWiFi"
#define PRESET_WIFI_PASSWORD "my-wifi-password"

// Optional: name on the network -> http://<name>.local (English letters, digits, '-')
// #define PRESET_HOSTNAME "electricity-meter"

// Optional: login for the web UI (password: at least 8 characters).
// Without these, the first visit to the web UI asks you to create a user.
// #define PRESET_ADMIN_USER "admin"
// #define PRESET_ADMIN_PASSWORD "choose-a-password"
