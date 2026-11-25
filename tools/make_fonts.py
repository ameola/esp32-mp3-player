#!/usr/bin/env python3
"""Generates UiFontData.cpp: the UI's fonts and icons as 4-bit anti-aliased
bitmaps, stored in flash and drawn by UiFont.cpp.

Text: Inter (SIL Open Font License, tools/fonts/OFL.txt), from
  https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz,wght%5D.ttf
Icons: Material Symbols Rounded (Apache License 2.0, tools/fonts/Apache-2.0.txt), from
  https://github.com/google/material-design-icons/raw/master/variablefont/
  MaterialSymbolsRounded%5BFILL,GRAD,opsz,wght%5D.ttf

Usage: python3 tools/make_fonts.py <Inter.ttf> <MaterialSymbolsRounded.ttf> [preview.png]
Needs Pillow (with FreeType). Writes UiFontData.cpp next to the sketch.
"""
import os
import sys

from PIL import Image, ImageFont

# name, pixel size, weight. Sizes follow the UI mockups (device pixels).
TEXT_FONTS = [
    ("FONT_TAB", 10, 500),          # tab labels
    ("FONT_LABEL", 10, 650),        # section labels, small buttons
    ("FONT_SMALL", 11, 400),        # secondary lines, times
    ("FONT_BODY", 13, 600),         # row titles
    ("FONT_TITLE", 15, 700),        # screen titles
    ("FONT_LARGE_SMALL", 12, 400),  # "Large text": secondary lines
    ("FONT_LARGE_BODY", 15, 600),   # "Large text": row titles
    ("FONT_HEADLINE", 18, 700),     # artist name, now playing title
]

# Code points every text font carries: ASCII, Latin-1 (for names once the
# library keeps accents), and the typographic marks tags often hold.
CHARS = (list(range(32, 127)) + list(range(160, 256)) +
         [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026])

# Icons: name -> (Material Symbols code point, filled)
ICONS = {
    "MUSIC_NOTE": (0xE405, False), "PERSON": (0xF0D3, False), "ALBUM": (0xE019, False),
    "FOLDER": (0xE2C7, False), "TUNE": (0xE429, False), "SEARCH": (0xEF7A, False),
    "BLUETOOTH": (0xE1A7, False), "BLUETOOTH_OFF": (0xE1A9, False),
    "WIFI": (0xE63E, False), "WIFI_OFF": (0xE648, False),
    "PLAY": (0xE037, True), "PAUSE": (0xE034, True), "NEXT": (0xE044, True), "PREVIOUS": (0xE045, True),
    "SHUFFLE": (0xE043, False), "REPEAT": (0xE040, False), "VOLUME": (0xE050, False),
    "HEADPHONES": (0xF01F, False), "LYRICS": (0xEC0B, False),
    "BACK": (0xE5CB, False), "CHEVRON": (0xE5CC, False), "EXPAND": (0xE5CF, False),
    "STACK": (0xF500, False), "CLOSE": (0xE5CD, False), "INFO": (0xE88E, False),
    "EQUALIZER": (0xE01D, True), "BRIGHTNESS": (0xE3AB, False), "IMAGE": (0xE3F4, False),
    "CHECK": (0xE668, False), "PALETTE": (0xE40A, False), "DENSITY": (0xEB9E, False),
    "TEXT_SIZE": (0xE245, False), "CORNER": (0xE920, False), "DOWNLOAD": (0xF090, False),
    "REFRESH": (0xE5D5, False), "MEMORY": (0xE322, False),
}
ICON_SIZES = [14, 18, 24]

GAMMA = 0.85  # < 1 thickens light-on-dark text a little


def quantize(a):
    return min(15, round(((a / 255.0) ** GAMMA) * 15))


def glyph_bitmap(img):
    w, h = img.size
    px = img.load()
    return [quantize(px[x, y]) for y in range(h) for x in range(w)], w, h


def trim(levels, w, h):
    """Drops empty rows/columns; returns levels, w, h, dx, dy."""
    if w == 0 or h == 0 or not any(levels):
        return [], 0, 0, 0, 0
    rows = [levels[y * w:(y + 1) * w] for y in range(h)]
    top = next(i for i, r in enumerate(rows) if any(r))
    bottom = max(i for i, r in enumerate(rows) if any(r))
    left = min(next(x for x, v in enumerate(r) if v) for r in rows if any(r))
    right = max(max(x for x, v in enumerate(r) if v) for r in rows if any(r))
    out = [rows[y][x] for y in range(top, bottom + 1) for x in range(left, right + 1)]
    return out, right - left + 1, bottom - top + 1, left, top


def pack(levels):
    out = []
    for i in range(0, len(levels), 2):
        hi = levels[i]
        lo = levels[i + 1] if i + 1 < len(levels) else 0
        out.append(hi << 4 | lo)
    return out


def text_font(path, px, weight):
    f = ImageFont.truetype(path, px)
    f.set_variation_by_axes([max(14, min(32, px)), weight])
    ascent, descent = f.getmetrics()
    glyphs = []
    digit_adv = max(round(f.getlength(str(d))) for d in range(10))
    for cp in CHARS:
        ch = chr(cp)
        adv = round(f.getlength(ch))
        mask, (ox, oy) = f.getmask2(ch, mode="L", anchor="ls")
        img = Image.frombytes("L", mask.size, bytes(mask)) if mask.size[0] and mask.size[1] else Image.new("L", (0, 0))
        levels, w, h = glyph_bitmap(img)
        levels, w, h, dx, dy = trim(levels, w, h)
        x, y = ox + dx, oy + dy  # bitmap top-left relative to the pen on the baseline
        if ch.isdigit():  # tabular digits: equal advances, glyph centred
            x += (digit_adv - adv) // 2
            adv = digit_adv
        glyphs.append((cp, adv, w, h, x, y, levels))
    return glyphs, ascent, descent


def icon_font(path, size):
    glyphs = []
    for name, (cp, filled) in sorted(ICONS.items(), key=lambda kv: kv[1][0]):
        f = ImageFont.truetype(path, size)
        f.set_variation_by_axes([1 if filled else 0, 0, max(20, min(48, size)), 400])
        mask, (ox, oy) = f.getmask2(chr(cp), mode="L", anchor="ls")
        img = Image.frombytes("L", mask.size, bytes(mask))
        levels, w, h = glyph_bitmap(img)
        levels, w, h, dx, dy = trim(levels, w, h)
        # Centre the drawing in a size x size box (x, y: from the box's top-left).
        glyphs.append((cp, size, w, h, (size - w) // 2, (size - h) // 2, levels))
    return glyphs, size, 0


def emit(name, glyphs, ascent, descent, out):
    bits, entries = [], []
    for cp, adv, w, h, x, y, levels in glyphs:
        entries.append((cp, adv, w, h, x, y, len(bits)))
        bits.extend(pack(levels))
    out.append(f"static const uint8_t {name}_bits[] = {{")
    for i in range(0, len(bits), 24):
        out.append("  " + ",".join(f"0x{b:02x}" for b in bits[i:i + 24]) + ",")
    out.append("};")
    out.append(f"static const UiGlyph {name}_glyphs[] = {{")
    for e in entries:
        out.append("  {0x%04x, %d, %d, %d, %d, %d, %d}," % e)
    out.append("};")
    out.append(f"const UiFont {name} = {{{name}_glyphs, {len(entries)}, {name}_bits, {ascent}, {descent}}};")
    out.append("")
    return len(bits) + len(entries) * 10


def preview(fonts, icons, path):
    """Draws sample text from the packed levels (what the device draws)."""
    img = Image.new("RGB", (480, 40 + 26 * len(fonts) + 40 * len(icons)), (15, 17, 21))
    px = img.load()
    y0 = 20
    sample = "Penny Lane · The Beatles 3:02 “Don’t” Beyoncé — 1234567890…"
    for name, (glyphs, ascent, descent) in fonts.items():
        table = {g[0]: g for g in glyphs}
        pen = 8
        for ch in sample:
            g = table.get(ord(ch), table[ord("?")])
            cp, adv, w, h, x, y, levels = g
            for j in range(h):
                for i in range(w):
                    a = levels[j * w + i] / 15
                    X, Y = pen + x + i, y0 + ascent + y + j
                    if 0 <= X < img.width and 0 <= Y < img.height:
                        bg = px[X, Y]
                        px[X, Y] = tuple(round(b + (236 - b) * a) for b in bg)
            pen += adv
        y0 += max(26, ascent + descent + 8)
    for size, (glyphs, _, _) in icons.items():
        pen = 8
        for cp, adv, w, h, x, y, levels in glyphs:
            for j in range(h):
                for i in range(w):
                    a = levels[j * w + i] / 15
                    X, Y = pen + x + i, y0 + y + j
                    if X < img.width and Y < img.height:
                        bg = px[X, Y]
                        px[X, Y] = tuple(round(b + (c - b) * a) for b, c in zip(bg, (244, 169, 60)))
            pen += size + 4
        y0 += size + 12
    img.save(path)


def main():
    inter, symbols = sys.argv[1], sys.argv[2]
    out = ["// Generated by tools/make_fonts.py: do not edit.",
           "// Inter: SIL Open Font License 1.1 (tools/fonts/OFL.txt).",
           "// Material Symbols: Apache License 2.0 (tools/fonts/Apache-2.0.txt).",
           '#include "UiFont.h"', ""]
    total = 0
    fonts, icons = {}, {}
    for name, px, weight in TEXT_FONTS:
        glyphs, ascent, descent = text_font(inter, px, weight)
        fonts[name] = (glyphs, ascent, descent)
        total += emit(name, glyphs, ascent, descent, out)
    for size in ICON_SIZES:
        glyphs, ascent, descent = icon_font(symbols, size)
        icons[size] = (glyphs, ascent, descent)
        total += emit(f"ICONS_{size}", glyphs, size, 0, out)
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(here, "UiFontData.cpp"), "w") as f:
        f.write("\n".join(out))
    # Icon code points for UiFont.h, kept in sync by hand from this list.
    print("// icons:", ", ".join(f"ICON_{n} = 0x{cp:04x}" for n, (cp, _) in sorted(ICONS.items())))
    print(f"UiFontData.cpp: {total} bytes of font data")
    if len(sys.argv) > 3:
        preview(fonts, icons, sys.argv[3])


if __name__ == "__main__":
    main()
