#pragma once
// Text conversion. Tag and file name text is kept as UTF-8 (the UI's fonts
// draw Latin-1 and typographic marks; other letters show as '?' but are
// kept). foldAscii() gives the plain-ASCII form used for sorting, grouping,
// cover keys and search, so "Beyonce" and "Beyoncé" agree.

#include <Arduino.h>

void appendCodepoint(String &out, uint32_t cp);
String decodeUtf8(const uint8_t *s, size_t n);
String decodeLatin1(const uint8_t *s, size_t n);
String decodeUtf16(const uint8_t *s, size_t n, bool bigEndian);  // honors a BOM

// Shortens text to maxChars characters (code points), ending in "..." when cut.
String fitText(const String &text, size_t maxChars);
// UTF-8 text with Latin-1 letters folded to their base letter, typographic
// quotes and dashes to ASCII, and anything else as '?'.
String foldAscii(const char *utf8);
// The length in bytes of the longest prefix of `s`, at most maxBytes, that
// doesn't split a UTF-8 character.
size_t utf8Prefix(const char *s, size_t len, size_t maxBytes);
