#pragma once
// Text conversion for the built-in GFX font, which is ASCII only. Latin-1
// accented letters fold to their base letter; anything else becomes '?'.

#include <Arduino.h>

void appendCodepoint(String &out, uint32_t cp);
String decodeUtf8(const uint8_t *s, size_t n);
String decodeLatin1(const uint8_t *s, size_t n);
String decodeUtf16(const uint8_t *s, size_t n, bool bigEndian);  // honors a BOM

// Shortens text to maxChars, ending in "..." when cut.
String fitText(const String &text, size_t maxChars);
