#pragma once
// Copy to secrets.h (git-ignored) and fill in. build.sh does the copy on first build.
static const char *WIFI_SSID = "your-home-wifi";      // network the dash joins when in range
static const char *WIFI_PASS = "your-wifi-password";
static const char *AP_SSID = "OBD-Probe";             // hotspot the dash starts when home WiFi isn't found
static const char *AP_PASS = "change-me-8+chars";     // WPA2 needs at least 8 characters
