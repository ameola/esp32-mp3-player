#pragma once
// On-screen keyboard (4 rows across the screen's bottom): letters, shift,
// two pages of digits and symbols, space, delete, back and an enter key with
// the caller's label ("Join", "Search"). The keyboard handles its own layer
// keys and highlights the key under the finger; callers get what was typed.
#include <Arduino.h>

#include "Touch.h"

#define KB_KEY_H 42
#define KB_H (4 * KB_KEY_H)

enum KeyAction : uint8_t { KEY_NONE, KEY_CHAR, KEY_DELETE, KEY_BACK, KEY_ENTER };
struct KeyPress {
  KeyAction action = KEY_NONE;
  char c = 0;  // KEY_CHAR
};

// Shows the keyboard with its top at y (lower case).
void keyboardShow(int16_t y, const char *enterLabel);
void keyboardDraw();
// Touches inside the keyboard: highlights on down/move, returns the key
// released on (KEY_NONE for layer keys, which it handles itself).
KeyPress keyboardTouch(TouchPhase phase, int16_t x, int16_t y);
