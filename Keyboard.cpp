#include "Keyboard.h"

#include <vector>

#include "Config.h"
#include "Lcd.h"
#include "UiFont.h"

namespace {

enum KeyCode : uint8_t { K_SHIFT = 1, K_DEL, K_LAYER, K_SYM2, K_BACK, K_ENTER };

struct Key {
  int16_t x, w;
  uint8_t code;       // a character, or a KeyCode
  const char *label;  // nullptr: the character itself
};

const char *const letterRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
const char *const symRows[2][3] = {
  {"1234567890", "-/:;()$&@", ".,?!'\"_"},
  {"[]{}#%^*+=", "\\|~<>`", ".,?!'\"_"},
};

int16_t topY = SCREEN_H - KB_H;
const char *enterText = "Enter";
int layer = 0;  // 0 lower, 1 upper (one letter), 2 digits/symbols, 3 more symbols
int pressedRow = -1, pressedKey = -1;

std::vector<Key> keyRow(int row) {
  std::vector<Key> keys;
  if (row == 3) {
    keys.push_back({0, 48, K_BACK, "Back"});
    keys.push_back({48, 48, K_LAYER, layer >= 2 ? "ABC" : "123"});
    keys.push_back({96, 72, ' ', "space"});
    keys.push_back({168, 72, K_ENTER, enterText});
    return keys;
  }
  const char *chars = layer >= 2 ? symRows[layer - 2][row] : letterRows[row];
  const int n = strlen(chars);
  int16_t x = row == 2 ? 36 : (SCREEN_W - n * 24) / 2;
  if (row == 2) {
    keys.push_back({0, 36, static_cast<uint8_t>(layer >= 2 ? K_SYM2 : K_SHIFT),
                    layer == 2 ? "#+=" : layer == 3 ? "123" : "shift"});
  }
  for (int i = 0; i < n; i++, x += 24) {
    const char c = layer == 1 ? toupper(chars[i]) : chars[i];
    keys.push_back({x, 24, static_cast<uint8_t>(c), nullptr});
  }
  if (row == 2) keys.push_back({204, 36, K_DEL, "del"});
  return keys;
}

void drawRow(int row) {
  canvas->fillRect(0, 0, SCREEN_W, KB_KEY_H, C_BG);
  const std::vector<Key> keys = keyRow(row);
  for (size_t i = 0; i < keys.size(); i++) {
    const Key &k = keys[i];
    const bool pressed = row == pressedRow && static_cast<int>(i) == pressedKey;
    const bool active = k.code == K_SHIFT && layer == 1;
    canvas->fillRoundRect(k.x + 2, 3, k.w - 4, KB_KEY_H - 6, uiStyle.square ? 1 : 5,
                          pressed ? C_BUTTON_PRESS : active ? C_TAB_ACTIVE : C_BUTTON);
    if (k.label) {
      const int16_t tw = uiTextWidth(FONT_LABEL, k.label);
      uiDrawText(FONT_LABEL, k.label, k.x + (k.w - tw) / 2, KB_KEY_H / 2 + 4, k.code == K_ENTER ? C_ACCENT : C_TEXT);
    } else {
      const char s[2] = {static_cast<char>(k.code), 0};
      uiDrawText(FONT_BODY, s, k.x + (k.w - uiTextWidth(FONT_BODY, s)) / 2, KB_KEY_H / 2 + 5, C_TEXT);
    }
  }
  lcdBlit(0, topY + row * KB_KEY_H, SCREEN_W, KB_KEY_H);
}

}  // namespace

void keyboardShow(int16_t y, const char *enterLabel) {
  topY = y;
  enterText = enterLabel;
  layer = 0;
  pressedRow = pressedKey = -1;
  keyboardDraw();
}

void keyboardDraw() {
  for (int r = 0; r < 4; r++) drawRow(r);
}

KeyPress keyboardTouch(TouchPhase phase, int16_t x, int16_t y) {
  KeyPress press;
  const int row = y >= topY ? (y - topY) / KB_KEY_H : -1;
  int key = -1;
  std::vector<Key> keys;
  if (row >= 0 && row < 4) {
    keys = keyRow(row);
    for (size_t i = 0; i < keys.size(); i++) {
      if (x >= keys[i].x && x < keys[i].x + keys[i].w) key = i;
    }
  }
  if (phase == TOUCH_DOWN || phase == TOUCH_MOVE) {
    if (row != pressedRow || key != pressedKey) {  // follow the finger
      const int oldRow = pressedRow;
      pressedRow = key >= 0 ? row : -1;
      pressedKey = key;
      if (oldRow >= 0 && oldRow != pressedRow) drawRow(oldRow);
      if (pressedRow >= 0) drawRow(pressedRow);
    }
    return press;
  }
  // TOUCH_UP: the key under the finger.
  const int r = pressedRow, k = pressedKey;
  pressedRow = pressedKey = -1;
  if (r < 0) return press;
  drawRow(r);
  if (r != row || k != key) return press;
  switch (keys[k].code) {
    case K_SHIFT: layer = layer == 1 ? 0 : 1; keyboardDraw(); break;
    case K_LAYER: layer = layer >= 2 ? 0 : 2; keyboardDraw(); break;
    case K_SYM2: layer = layer == 2 ? 3 : 2; keyboardDraw(); break;
    case K_DEL: press.action = KEY_DELETE; break;
    case K_BACK: press.action = KEY_BACK; break;
    case K_ENTER: press.action = KEY_ENTER; break;
    default:
      press.action = KEY_CHAR;
      press.c = static_cast<char>(keys[k].code);
      if (layer == 1) {  // shift applies to one letter
        layer = 0;
        keyboardDraw();
      }
      break;
  }
  return press;
}
