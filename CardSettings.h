#pragma once
// Per-card settings in LIB_DIR "/settings.txt" (plain "key=value" lines).
// A card without this file hasn't been set up yet: the player asks for WiFi
// details (for album art) before building the library.
#include <Arduino.h>

struct CardSettings {
  bool artDownload = false;  // download missing album art over WiFi
};

// False if the card has no settings file (not set up yet).
bool cardSettingsLoad(CardSettings &out);
bool cardSettingsSave(const CardSettings &s);
