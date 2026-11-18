#pragma once
// FT6336U capacitive touch controller, polled over I2C (CTP_INT unconnected).

#include <Arduino.h>

void touchInit();

enum TouchPhase { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

// Returns true when something happened: finger down, moved (while held) or
// lifted, with screen coordinates (for TOUCH_UP, the last position).
// Polls at most every 25 ms; cheap to call every loop.
bool touchPoll(int16_t &sx, int16_t &sy, TouchPhase &phase);

// millis() of the last poll that saw the finger down but not moving (0 if
// none since it went down). Tells a finger that stopped before lifting from
// one whose movement just wasn't polled because the UI was busy.
uint32_t touchLastStillPoll();
