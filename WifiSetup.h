#pragma once
// First-run WiFi setup screen (for album art downloads): a list of nearby
// networks, an on-screen keyboard for the password, and a connection test.
// The user can skip it. Credentials are kept in NVS (the ESP32's own flash),
// so they survive card swaps; the WiFi radio is off again when it returns.
#include <Arduino.h>
#include "Touch.h"

enum WifiSetupResult { WS_RUNNING, WS_CONNECTED, WS_SKIPPED };

void wifiSetupBegin();  // takes over the screen and starts a network scan
void wifiSetupTouch(TouchPhase phase, int16_t x, int16_t y);
WifiSetupResult wifiSetupTick();  // call every loop while it runs

// Network saved by the last successful setup (false if none).
bool wifiSavedCredentials(String &ssid, String &password);
