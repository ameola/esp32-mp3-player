#include "UiTheme.h"

#include <Preferences.h>

#include "Lcd.h"

namespace {

// The themes from the UI mockups (Settings > Display).
const UiPalette kThemes[THEME_COUNT] = {
  // Night radio: near-black, amber
  {rgb565(0x0F1115), rgb565(0x181B21), rgb565(0x22262E), rgb565(0x2A2F38), rgb565(0xECEEF1), rgb565(0xA0A7B1),
   rgb565(0x7C838D), rgb565(0xF4A93C), rgb565(0x1A1206), rgb565(0x1D1A14)},
  // Deep sea: blue-black, teal
  {rgb565(0x0A1018), rgb565(0x111A25), rgb565(0x1A2533), rgb565(0x233142), rgb565(0xE6EEF5), rgb565(0x93A4B5),
   rgb565(0x6E8093), rgb565(0x3DD6C3), rgb565(0x032420), rgb565(0x0E2226)},
  // Graphite: neutral greys, coral
  {rgb565(0x121212), rgb565(0x1C1C1C), rgb565(0x262626), rgb565(0x303030), rgb565(0xEDEDED), rgb565(0xA3A3A3),
   rgb565(0x7A7A7A), rgb565(0xFF6B5E), rgb565(0x2B0B07), rgb565(0x251816)},
  // Cover colour: replaced by uiThemeCover(); Night radio until a cover shows
  {rgb565(0x0F1115), rgb565(0x181B21), rgb565(0x22262E), rgb565(0x2A2F38), rgb565(0xECEEF1), rgb565(0xA0A7B1),
   rgb565(0x7C838D), rgb565(0xF4A93C), rgb565(0x1A1206), rgb565(0x1D1A14)},
};
const char *const kThemeNames[THEME_COUNT] = {"Night radio", "Deep sea", "Graphite", "Cover colour"};

UiPalette coverPal = kThemes[THEME_COVER];

// HSV (h 0..360, s and v 0..1) to RGB565.
uint16_t hsv(float h, float s, float v) {
  const float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
  float r = 0, g = 0, b = 0;
  if (h < 60) r = c, g = x;
  else if (h < 120) r = x, g = c;
  else if (h < 180) g = c, b = x;
  else if (h < 240) g = x, b = c;
  else if (h < 300) r = x, b = c;
  else r = c, b = x;
  const uint32_t rgb = static_cast<uint32_t>((r + m) * 255) << 16 | static_cast<uint32_t>((g + m) * 255) << 8 |
                       static_cast<uint32_t>((b + m) * 255);
  return rgb565(rgb);
}

void apply() {
  pal = uiStyle.theme == THEME_COVER ? coverPal : kThemes[min<uint8_t>(uiStyle.theme, THEME_COUNT - 1)];
}

}  // namespace

UiPalette pal = kThemes[THEME_NIGHT_RADIO];
UiStyle uiStyle;

void uiStyleLoad() {
  Preferences p;
  if (p.begin("ui", true)) {
    uiStyle.theme = min<uint8_t>(p.getUChar("theme", THEME_NIGHT_RADIO), THEME_COUNT - 1);
    uiStyle.compact = p.getBool("compact", false);
    uiStyle.large = p.getBool("large", false);
    uiStyle.square = p.getBool("square", false);
    uiStyle.brightness = min<uint8_t>(p.getUChar("bright", 3), 3);
    uiStyle.sleep = min<uint8_t>(p.getUChar("sleep", 4), 4);
    p.end();
  }
  apply();
  lcdSetBacklight(uiBrightnessPercent(uiStyle.brightness));
}

void uiStyleSet(const UiStyle &style) {
  uiStyle = style;
  apply();
  lcdSetBacklight(uiBrightnessPercent(uiStyle.brightness));
  Preferences p;
  if (p.begin("ui", false)) {
    p.putUChar("theme", uiStyle.theme);
    p.putBool("compact", uiStyle.compact);
    p.putBool("large", uiStyle.large);
    p.putBool("square", uiStyle.square);
    p.putUChar("bright", uiStyle.brightness);
    p.putUChar("sleep", uiStyle.sleep);
    p.end();
  }
}

const char *uiThemeName(uint8_t theme) {
  return kThemeNames[min<uint8_t>(theme, THEME_COUNT - 1)];
}

bool uiThemeCover(const uint16_t *px) {
  UiPalette next = kThemes[THEME_COVER];
  if (px) {
    // The cover's main hue: hues of its colourful pixels, weighted by how
    // colourful they are (a circular mean, so reds either side of 0 agree).
    float sx = 0, sy = 0, weight = 0;
    for (int i = 0; i < 30 * 30; i++) {
      const float r = ((px[i] >> 11) & 31) / 31.0f, g = ((px[i] >> 5) & 63) / 63.0f, b = (px[i] & 31) / 31.0f;
      const float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
      if (d < 0.15f || mx < 0.2f) continue;  // greys, near-black
      float h = mx == r ? 60 * fmodf((g - b) / d, 6) : mx == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
      if (h < 0) h += 360;
      const float w = d * mx;
      sx += w * cosf(h * DEG_TO_RAD);
      sy += w * sinf(h * DEG_TO_RAD);
      weight += w;
    }
    if (weight > 20) {  // colourful enough to theme by
      float h = atan2f(sy, sx) * RAD_TO_DEG;
      if (h < 0) h += 360;
      // Accent bright enough to read on the dark ground; ground and
      // surfaces a faint tint of the same hue.
      next = {hsv(h, 0.40f, 0.085f), hsv(h, 0.38f, 0.13f), hsv(h, 0.34f, 0.18f), hsv(h, 0.30f, 0.23f),
              hsv(h, 0.05f, 0.96f),  hsv(h, 0.14f, 0.73f), hsv(h, 0.16f, 0.56f), hsv(h, 0.62f, 0.95f),
              hsv(h, 0.80f, 0.16f),  hsv(h, 0.45f, 0.17f)};
    }
  }
  const bool changed = memcmp(&next, &coverPal, sizeof(next)) != 0;
  coverPal = next;
  if (uiStyle.theme == THEME_COVER) apply();
  return changed && uiStyle.theme == THEME_COVER;
}

const char *uiBrightnessName(uint8_t level) {
  static const char *const names[] = {"Low", "Medium", "High", "Max"};
  return names[min<uint8_t>(level, 3)];
}

uint8_t uiBrightnessPercent(uint8_t level) {
  static const uint8_t percent[] = {30, 55, 80, 100};
  return percent[min<uint8_t>(level, 3)];
}

const char *uiSleepName(uint8_t index) {
  static const char *const names[] = {"30 s", "1 min", "5 min", "15 min", "Never"};
  return names[min<uint8_t>(index, 4)];
}

uint32_t uiSleepMs(uint8_t index) {
  static const uint32_t ms[] = {30000, 60000, 300000, 900000, 0};
  return ms[min<uint8_t>(index, 4)];
}

int16_t uiRowH() {
  return (uiStyle.compact ? 34 : 40) + (uiStyle.large ? 6 : 0);
}

const UiFont &uiFontBody() {
  return uiStyle.large ? FONT_LARGE_BODY : FONT_BODY;
}

const UiFont &uiFontSmall() {
  return uiStyle.large ? FONT_LARGE_SMALL : FONT_SMALL;
}

uint8_t uiRadiusArt() {
  return uiStyle.square ? 0 : 4;
}

uint8_t uiRadiusCard() {
  return uiStyle.square ? 0 : 8;
}

uint8_t uiRadiusPill(uint8_t h) {
  return uiStyle.square ? 2 : h / 2;
}
