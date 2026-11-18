#include "Text.h"

void appendCodepoint(String &out, uint32_t cp) {
  static const char latin1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"   // U+00C0..U+00DF
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";  // U+00E0..U+00FF
  if (cp >= 0x20 && cp < 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp >= 0xC0 && cp <= 0xFF) {
    out += latin1[cp - 0xC0];
  } else if (cp == 0x2018 || cp == 0x2019) {
    out += '\'';
  } else if (cp == 0x201C || cp == 0x201D) {
    out += '"';
  } else if (cp == 0x2013 || cp == 0x2014) {
    out += '-';
  } else if (cp >= 0x80) {
    out += '?';
  }
}

String decodeUtf8(const uint8_t *s, size_t n) {
  String out;
  size_t i = 0;
  while (i < n && s[i] != 0) {
    const uint8_t c = s[i];
    uint32_t cp;
    size_t extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { i++; continue; }  // stray continuation byte
    i++;
    for (size_t k = 0; k < extra && i < n && (s[i] & 0xC0) == 0x80; k++, i++) {
      cp = (cp << 6) | (s[i] & 0x3F);
    }
    appendCodepoint(out, cp);
  }
  return out;
}

String decodeLatin1(const uint8_t *s, size_t n) {
  String out;
  for (size_t i = 0; i < n && s[i] != 0; i++) {
    appendCodepoint(out, s[i]);
  }
  return out;
}

String decodeUtf16(const uint8_t *s, size_t n, bool bigEndian) {
  if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { bigEndian = false; s += 2; n -= 2; }
  else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { bigEndian = true; s += 2; n -= 2; }
  String out;
  for (size_t i = 0; i + 1 < n; i += 2) {
    const uint16_t unit = bigEndian ? (s[i] << 8) | s[i + 1] : (s[i + 1] << 8) | s[i];
    if (unit == 0) {
      break;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      continue;  // low surrogate; its high half already printed '?'
    }
    appendCodepoint(out, unit);
  }
  return out;
}

String fitText(const String &text, size_t maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }
  return text.substring(0, maxChars - 3) + "...";
}
