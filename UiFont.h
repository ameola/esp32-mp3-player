#pragma once
// Anti-aliased text and icons for the UI. Glyphs are 4-bit coverage bitmaps
// in flash (UiFontData.cpp, made by tools/make_fonts.py from Inter and
// Material Symbols); drawing blends each pixel with what is already in the
// canvas, so text sits cleanly on any background, tinted rows included.
#include <Arduino.h>

struct UiGlyph {
  uint16_t code;   // Unicode code point (icons: Material Symbols code point)
  uint8_t advance; // pen movement
  uint8_t w, h;    // bitmap size
  int8_t x, y;     // bitmap top-left from the pen on the baseline (icons: from the box's top-left)
  uint32_t offset; // into bits, 2 pixels per byte, high nibble first
};

struct UiFont {
  const UiGlyph *glyphs;  // sorted by code; text fonts start with ASCII 32..126
  uint16_t count;
  const uint8_t *bits;
  uint8_t ascent, descent;  // icon fonts: ascent = box size
};

extern const UiFont FONT_TAB, FONT_LABEL, FONT_SMALL, FONT_BODY, FONT_TITLE, FONT_LARGE_SMALL,
  FONT_LARGE_BODY, FONT_HEADLINE;
extern const UiFont ICONS_14, ICONS_18, ICONS_24;

enum UiIcon : uint16_t {
  ICON_ALBUM = 0xe019, ICON_BACK = 0xe5cb, ICON_BLUETOOTH = 0xe1a7, ICON_BLUETOOTH_OFF = 0xe1a9,
  ICON_BRIGHTNESS = 0xe3ab, ICON_CHECK = 0xe668, ICON_CHEVRON = 0xe5cc, ICON_CLOSE = 0xe5cd,
  ICON_EQUALIZER = 0xe01d, ICON_EXPAND = 0xe5cf, ICON_FOLDER = 0xe2c7, ICON_HEADPHONES = 0xf01f,
  ICON_IMAGE = 0xe3f4, ICON_INFO = 0xe88e, ICON_LYRICS = 0xec0b, ICON_MUSIC_NOTE = 0xe405,
  ICON_NEXT = 0xe044, ICON_PAUSE = 0xe034, ICON_PERSON = 0xf0d3, ICON_PLAY = 0xe037,
  ICON_PREVIOUS = 0xe045, ICON_REPEAT = 0xe040, ICON_SEARCH = 0xef7a, ICON_SHUFFLE = 0xe043,
  ICON_STACK = 0xf500, ICON_TUNE = 0xe429, ICON_VOLUME = 0xe050, ICON_WIFI = 0xe63e,
  ICON_WIFI_OFF = 0xe648, ICON_PALETTE = 0xe40a, ICON_DENSITY = 0xeb9e, ICON_TEXT_SIZE = 0xe245,
  ICON_CORNER = 0xe920, ICON_DOWNLOAD = 0xf090, ICON_REFRESH = 0xe5d5, ICON_MEMORY = 0xe322,
  ICON_BEDTIME = 0xf159,
};

// Width in pixels of UTF-8 text.
int16_t uiTextWidth(const UiFont &f, const char *text);
// The text, shortened with "…" if wider than maxW.
String uiFitText(const UiFont &f, const String &text, int16_t maxW);

// Draws UTF-8 text into the canvas with the pen starting at (x, baseline),
// only between clipX0 and clipX1 (for scrolling titles). Returns the pen's
// x after the text.
int16_t uiDrawText(const UiFont &f, const char *text, int16_t x, int16_t baseline, uint16_t color,
                   int16_t clipX0 = 0, int16_t clipX1 = 32767);
// Same, the text's top at y (baseline = y + ascent).
inline int16_t uiDrawTextTop(const UiFont &f, const char *text, int16_t x, int16_t y, uint16_t color) {
  return uiDrawText(f, text, x, y + f.ascent, color);
}
// Draws an icon into the canvas, centred in the icon font's box at (x, y).
void uiDrawIcon(const UiFont &icons, uint16_t icon, int16_t x, int16_t y, uint16_t color);

// RGB565 helpers for themes.
constexpr uint16_t rgb565(uint32_t rgb) {
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}
uint16_t uiBlend(uint16_t bg, uint16_t fg, uint8_t alpha15);  // alpha 0..15
