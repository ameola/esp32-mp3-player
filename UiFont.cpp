#include "UiFont.h"

#include "Lcd.h"

namespace {

// Next code point of UTF-8 text; malformed bytes come out as '?'.
uint32_t nextCodepoint(const char *&s) {
  const uint8_t c = *s++;
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return '?';
  uint32_t cp = c & (0x3F >> extra);
  while (extra--) {
    const uint8_t n = *s;
    if ((n & 0xC0) != 0x80) return '?';
    cp = cp << 6 | (n & 0x3F);
    s++;
  }
  return cp;
}

const UiGlyph *findGlyph(const UiFont &f, uint32_t cp) {
  if (cp >= 32 && cp < 127 && f.count >= 95 && f.glyphs[cp - 32].code == cp) return &f.glyphs[cp - 32];
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (f.glyphs[mid].code == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].code < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

const UiGlyph *glyphOrFallback(const UiFont &f, uint32_t cp) {
  const UiGlyph *g = findGlyph(f, cp);
  return g ? g : findGlyph(f, '?');
}

// Draws one glyph's bitmap with its top-left at (x, y), clipped to the
// canvas and to [clipX0, clipX1).
void blitGlyph(const UiFont &f, const UiGlyph &g, int16_t x, int16_t y, uint16_t color, int16_t clipX0,
               int16_t clipX1) {
  uint16_t *buf = canvas->getBuffer();
  const int16_t cw = canvas->width(), ch = canvas->height();
  const int16_t x0 = max<int16_t>(max<int16_t>(clipX0, 0), x), x1 = min<int16_t>(min<int16_t>(clipX1, cw), x + g.w);
  const int16_t y0 = max<int16_t>(0, y), y1 = min<int16_t>(ch, y + g.h);
  if (x0 >= x1 || y0 >= y1) return;
  const uint8_t *bits = f.bits + g.offset;
  for (int16_t py = y0; py < y1; py++) {
    uint16_t *row = buf + py * cw;
    uint32_t i = (py - y) * g.w + (x0 - x);
    for (int16_t px = x0; px < x1; px++, i++) {
      const uint8_t a = (i & 1) ? bits[i >> 1] & 0x0F : bits[i >> 1] >> 4;
      if (a == 15) row[px] = color;
      else if (a) row[px] = uiBlend(row[px], color, a);
    }
  }
}

}  // namespace

uint16_t uiBlend(uint16_t bg, uint16_t fg, uint8_t a) {
  // x / 15 as (x * 273 + 2048) >> 12: exact over the range used, no division.
  const uint32_t na = 15 - a;
  const uint32_t r = ((bg >> 11) * na + (fg >> 11) * a) * 273 + 2048 >> 12;
  const uint32_t gr = (((bg >> 5) & 0x3F) * na + ((fg >> 5) & 0x3F) * a) * 273 + 2048 >> 12;
  const uint32_t b = ((bg & 0x1F) * na + (fg & 0x1F) * a) * 273 + 2048 >> 12;
  return r << 11 | gr << 5 | b;
}

int16_t uiTextWidth(const UiFont &f, const char *s) {
  int16_t w = 0;
  while (*s) {
    const UiGlyph *g = glyphOrFallback(f, nextCodepoint(s));
    if (g) w += g->advance;
  }
  return w;
}

String uiFitText(const UiFont &f, const String &text, int16_t maxW) {
  if (uiTextWidth(f, text.c_str()) <= maxW) return text;
  static const char kEllipsis[] = "\xE2\x80\xA6";
  const int16_t room = maxW - uiTextWidth(f, kEllipsis);
  // Whole code points only: walk forward while it fits.
  const char *s = text.c_str(), *end = s;
  int16_t w = 0;
  while (*end) {
    const char *next = end;
    const UiGlyph *g = glyphOrFallback(f, nextCodepoint(next));
    const int16_t adv = g ? g->advance : 0;
    if (w + adv > room) break;
    w += adv;
    end = next;
  }
  while (end > s && end[-1] == ' ') end--;  // no space before the "…"
  return text.substring(0, end - s) + kEllipsis;
}

int16_t uiDrawText(const UiFont &f, const char *s, int16_t x, int16_t baseline, uint16_t color, int16_t clipX0,
                   int16_t clipX1) {
  while (*s) {
    const UiGlyph *g = glyphOrFallback(f, nextCodepoint(s));
    if (!g) continue;
    if (x >= clipX1) break;
    if (g->w && x + g->x + g->w > clipX0) blitGlyph(f, *g, x + g->x, baseline + g->y, color, clipX0, clipX1);
    x += g->advance;
  }
  return x;
}

void uiDrawIcon(const UiFont &icons, uint16_t icon, int16_t x, int16_t y, uint16_t color) {
  const UiGlyph *g = findGlyph(icons, icon);
  if (g && g->w) blitGlyph(icons, *g, x + g->x, y + g->y, color, 0, 32767);
}
