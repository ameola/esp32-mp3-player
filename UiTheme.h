#pragma once
// The UI's look as settings: a colour theme and three layout choices,
// saved on the device (NVS) and applied at once. Screens use only the names
// here (pal.text, uiRowH(), ...), never fixed values, so every theme and
// layout fits every screen.
#include <Arduino.h>

#include "UiFont.h"

// One theme: ten colours (RGB565).
struct UiPalette {
  uint16_t bg;        // ground
  uint16_t surface;   // raised: mini player, cards
  uint16_t surface2;  // raised more: buttons, placeholders
  uint16_t line;      // hairlines, empty progress
  uint16_t text;
  uint16_t dim;       // secondary text
  uint16_t faint;     // inactive icons, letter strip
  uint16_t accent;    // what's playing, active tab, primary buttons, live connections
  uint16_t onAccent;  // text and icons on the accent
  uint16_t playing;   // playing row tint
};

enum UiThemeId : uint8_t { THEME_NIGHT_RADIO, THEME_DEEP_SEA, THEME_GRAPHITE, THEME_COVER, THEME_COUNT };

struct UiStyle {
  uint8_t theme = THEME_NIGHT_RADIO;
  bool compact = false;  // density: Comfortable (5 rows) / Compact (6)
  bool large = false;    // text size: Standard / Large
  bool square = false;   // corners: Round / Square
  uint8_t brightness = 3;  // 0..3: Low, Medium, High, Max
  uint8_t sleep = 4;       // screen off after: 30 s, 1 min, 5 min, 15 min, Never
};

extern UiPalette pal;  // the current theme's colours
extern UiStyle uiStyle;

void uiStyleLoad();                       // once at start
void uiStyleSet(const UiStyle &style);    // applies and saves
const char *uiThemeName(uint8_t theme);
const char *uiBrightnessName(uint8_t level);
uint8_t uiBrightnessPercent(uint8_t level);
const char *uiSleepName(uint8_t index);
uint32_t uiSleepMs(uint8_t index);  // 0: never

// "Cover colour" theme: colours from the playing album's 30 px thumbnail
// (nullptr: no cover, fall back to Night radio). Returns true if the
// palette changed (screens should redraw).
bool uiThemeCover(const uint16_t *thumbPixels);

// Layout values the screens read.
int16_t uiRowH();                 // list row height
const UiFont &uiFontBody();       // row titles
const UiFont &uiFontSmall();      // secondary lines
uint8_t uiRadiusArt();            // covers in rows
uint8_t uiRadiusCard();           // cards, panels
uint8_t uiRadiusPill(uint8_t h);  // buttons of height h
