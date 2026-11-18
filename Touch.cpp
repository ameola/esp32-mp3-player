#include "Touch.h"

#include <Wire.h>
#include <utility>

#include "Config.h"

static bool writeTouchRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readTouchRegisters(uint8_t reg, uint8_t *data, uint8_t length) {
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(TOUCH_ADDRESS), length) != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (uint8_t index = 0; index < length; index++) {
    data[index] = Wire.read();
  }
  return true;
}

void touchInit() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(100000);
  const bool ok = writeTouchRegister(0x01, 0x01) && writeTouchRegister(0xF9, 0x01);
  Serial.print("Touch controller: ");
  Serial.println(ok ? "OK" : "no response");
}

static void toScreen(int16_t rx, int16_t ry, int16_t &sx, int16_t &sy) {
  int16_t x = rx;
  int16_t y = ry;
  if (TOUCH_SWAP_XY) {
    std::swap(x, y);
  }
  if (TOUCH_INVERT_X) {
    x = SCREEN_W - 1 - x;
  }
  if (TOUCH_INVERT_Y) {
    y = SCREEN_H - 1 - y;
  }
  sx = constrain(x, 0, SCREEN_W - 1);
  sy = constrain(y, 0, SCREEN_H - 1);
}

static uint32_t stillAt = 0;

uint32_t touchLastStillPoll() {
  return stillAt;
}

bool touchPoll(int16_t &sx, int16_t &sy, TouchPhase &phase) {
  static bool down = false;
  static int16_t lastX = 0, lastY = 0;
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 25) {
    return false;
  }
  lastPoll = now;

  uint8_t data[5];  // 0x02 status, 0x03..0x06 first point
  if (!readTouchRegisters(0x02, data, sizeof(data))) {
    return false;
  }
  const uint8_t count = data[0] & 0x0F;
  if (count == 0 || count > 2) {
    if (!down) {
      return false;
    }
    down = false;
    sx = lastX;
    sy = lastY;
    phase = TOUCH_UP;
    return true;
  }
  const int16_t rx = ((data[1] & 0x0F) << 8) | data[2];
  const int16_t ry = ((data[3] & 0x0F) << 8) | data[4];
  int16_t x, y;
  toScreen(rx, ry, x, y);
  if (!down) {
    down = true;
    stillAt = 0;
    phase = TOUCH_DOWN;
    Serial.printf("Tap raw=(%d,%d) screen=(%d,%d)\n", rx, ry, x, y);
  } else if (abs(x - lastX) > 2 || abs(y - lastY) > 2) {
    phase = TOUCH_MOVE;
  } else {
    stillAt = now | 1;
    return false;
  }
  lastX = sx = x;
  lastY = sy = y;
  return true;
}
