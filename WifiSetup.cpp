#include "WifiSetup.h"

#include <Preferences.h>
#include <WiFi.h>
#include <vector>

#include "Config.h"
#include "Lcd.h"
#include "UiFont.h"
#include "Text.h"

namespace {

// ---------------------------------------------------------------- layout
#define WS_HEADER_H 46               // its own header (the player's is 32)
#define WS_INTRO_Y WS_HEADER_H       // 46: two lines of explanation
#define WS_ROWS_Y 90                 // network rows
#define WS_ROW_H 32
#define WS_ROWS 5                    // 90 .. 250
#define WS_STATUS_Y (WS_ROWS_Y + WS_ROW_H * WS_ROWS)  // 250: "page 1 of 2"
#define WS_BUTTONS_Y (SCREEN_H - CANVAS_H)            // 276
#define WS_FIELD_Y 96                // password field
#define WS_KEY_H 42
#define WS_KEYS_Y (SCREEN_H - 4 * WS_KEY_H)           // 152
#define WS_CONNECT_TIMEOUT_MS 20000
#define WS_MAX_NETWORKS 30
#define WS_MAX_PASSWORD 63

enum Screen { S_LIST, S_PASSWORD, S_CONNECTING, S_MESSAGE };
enum KeyCode : uint8_t { K_SHIFT = 1, K_DEL, K_LAYER, K_SYM2, K_BACK, K_JOIN };

struct Net {
  String ssid;
  int8_t rssi;
  bool secure;
};

struct Key {
  int16_t x, w;
  uint8_t code;  // a character, or a KeyCode
  const char *label;  // nullptr: the character itself
};

Screen screen = S_LIST;
WifiSetupResult result = WS_RUNNING;
std::vector<Net> nets;
bool scanning = false;
uint32_t page = 0;
String chosen, password;
bool usingSaved = false;  // trying the saved password, no keyboard shown
int layer = 0;            // 0 lower, 1 upper, 2 digits/symbols, 3 more symbols
int pressedRow = -1, pressedKey = -1, pressedButton = -1, pressedNet = -1;
uint32_t connectStart = 0;
uint32_t messageUntil = 0;
Screen afterMessage = S_LIST;
WifiSetupResult resultAfterMessage = WS_RUNNING;

// ---------------------------------------------------------------- drawing helpers
// y is the top of the text: size 2 is body text, size 1 small text.
const UiFont &fontFor(uint8_t size) {
  return size >= 2 ? FONT_BODY : FONT_SMALL;
}

void text(const String &s, int16_t x, int16_t y, uint8_t size, uint16_t color) {
  uiDrawText(fontFor(size), s.c_str(), x, y + (size >= 2 ? 12 : 9), color);
}

void centered(const String &s, int16_t cx, int16_t y, uint8_t size, uint16_t color) {
  text(s, cx - uiTextWidth(fontFor(size), s.c_str()) / 2, y, size, color);
}

String display(const String &s) {  // SSIDs are UTF-8; the font is ASCII
  return decodeUtf8(reinterpret_cast<const uint8_t *>(s.c_str()), s.length());
}

void button(int16_t x, int16_t w, int16_t h, const char *label, bool pressed, bool enabled = true) {
  canvas->fillRoundRect(x + 4, 6, w - 8, h - 12, uiStyle.square ? 2 : (h - 12) / 2, pressed ? C_BUTTON_PRESS : C_BUTTON);
  const int16_t tw = uiTextWidth(FONT_LABEL, label);
  uiDrawText(FONT_LABEL, label, x + (w - tw) / 2, h / 2 + 4, enabled ? C_TEXT : C_DIM);
}

void drawHeader(const char *title) {
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  uiDrawText(FONT_TITLE, title, 10, 29, C_TEXT);
  lcdBlit(0, 0, SCREEN_W, WS_HEADER_H);
}

void drawTwoLines(int16_t y, const String &a, const String &b, uint16_t colorA = C_DIM) {
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  centered(fitText(a, 38), SCREEN_W / 2, 8, 1, colorA);
  centered(fitText(b, 38), SCREEN_W / 2, 22, 1, C_DIM);
  lcdBlit(0, y, SCREEN_W, CANVAS_H);
}

// Bottom row of up to three buttons.
const char *buttonLabels[3];
int buttonCount = 0;

void drawButtons() {
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  if (buttonCount) {
    const int16_t w = SCREEN_W / buttonCount;
    for (int i = 0; i < buttonCount; i++) {
      button(i * w, w, CANVAS_H, buttonLabels[i], i == pressedButton);
    }
  }
  lcdBlit(0, WS_BUTTONS_Y, SCREEN_W, CANVAS_H);
}

void setButtons(std::initializer_list<const char *> labels) {
  buttonCount = 0;
  for (const char *l : labels) buttonLabels[buttonCount++] = l;
  pressedButton = -1;
  drawButtons();
}

int buttonAt(int16_t x, int16_t y) {
  if (y < WS_BUTTONS_Y || !buttonCount) return -1;
  return min(x / (SCREEN_W / buttonCount), buttonCount - 1);
}

// ---------------------------------------------------------------- network list
uint32_t pageCount() { return max<uint32_t>(1, (nets.size() + WS_ROWS - 1) / WS_ROWS); }

String savedSsid() {
  String s, p;
  return wifiSavedCredentials(s, p) ? s : String();
}

void drawNetRow(int r) {
  const uint32_t i = page * WS_ROWS + r;
  canvas->fillRect(0, 0, SCREEN_W, WS_ROW_H, r == pressedNet ? C_BUTTON_PRESS : (r & 1 ? C_ROW_ALT : C_BG));
  if (i < nets.size()) {
    const Net &n = nets[i];
    const bool saved = n.ssid == savedSsid();
    text(uiFitText(FONT_BODY, display(n.ssid), saved ? 132 : 172), 10, 8, 2, C_TEXT);
    if (saved) uiDrawText(FONT_LABEL, "SAVED", 146, 20, C_ACCENT);
    // Signal: 4 bars; lock: a small padlock.
    const int bars = n.rssi > -55 ? 4 : n.rssi > -67 ? 3 : n.rssi > -75 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
      const int16_t h = 4 + b * 4;
      canvas->fillRect(204 + b * 5, 24 - h, 3, h, b < bars ? C_TEXT : C_PROGRESS_BG);
    }
    if (n.secure) {
      canvas->drawRoundRect(190, 12, 7, 8, 3, C_DIM);
      canvas->fillRect(188, 16, 11, 9, C_DIM);
    }
  } else if (r == 0) {
    centered(scanning ? "Searching for networks..." : "No networks found", SCREEN_W / 2, 12, 1, C_DIM);
  }
  lcdBlit(0, WS_ROWS_Y + r * WS_ROW_H, SCREEN_W, WS_ROW_H);
}

void drawListStatus() {
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  if (!scanning && nets.size() > WS_ROWS) {
    centered(String(nets.size()) + " networks, page " + (page + 1) + " of " + pageCount(), SCREEN_W / 2, 9, 1,
             C_DIM);
  }
  lcdBlit(0, WS_STATUS_Y, SCREEN_W, 26);
}

void drawListButtons() {
  if (nets.size() > WS_ROWS) setButtons({"Rescan", "More", "Skip"});
  else setButtons({"Rescan", "Skip"});
}

void showList() {
  screen = S_LIST;
  pressedNet = -1;
  lcdFillRect(0, WS_HEADER_H, SCREEN_W, SCREEN_H - WS_HEADER_H, C_BG);
  drawHeader("Album art");
  drawTwoLines(WS_INTRO_Y, "Pick a WiFi network so the player can", "download album art. Or tap Skip.", C_TEXT);
  for (int r = 0; r < WS_ROWS; r++) drawNetRow(r);
  drawListStatus();
  drawListButtons();
}

void startScan() {
  nets.clear();
  page = 0;
  scanning = true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.scanNetworks(true);  // async; polled in wifiSetupTick
  if (screen == S_LIST) showList();
}

void collectScan(int count) {
  for (int i = 0; i < count && nets.size() < WS_MAX_NETWORKS; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden network
    bool dup = false;
    for (Net &n : nets) {
      if (n.ssid == ssid) {  // same network on several access points
        n.rssi = max<int8_t>(n.rssi, WiFi.RSSI(i));
        dup = true;
      }
    }
    if (!dup) nets.push_back({ssid, static_cast<int8_t>(WiFi.RSSI(i)), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  // The saved network first, if it's here.
  const String saved = savedSsid();
  for (size_t i = 1; i < nets.size(); i++) {
    if (nets[i].ssid == saved) std::rotate(nets.begin(), nets.begin() + i, nets.begin() + i + 1);
  }
}

// ---------------------------------------------------------------- keyboard
const char *const letterRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
const char *const symRows[2][3] = {
  {"1234567890", "-/:;()$&@", ".,?!'\"_"},
  {"[]{}#%^*+=", "\\|~<>`", ".,?!'\"_"},
};

// Keys of one keyboard row in the current layer.
std::vector<Key> keyRow(int row) {
  std::vector<Key> keys;
  if (row == 3) {
    keys.push_back({0, 48, K_BACK, "Back"});
    keys.push_back({48, 48, K_LAYER, layer >= 2 ? "ABC" : "123"});
    keys.push_back({96, 72, ' ', "space"});
    keys.push_back({168, 72, K_JOIN, "Join"});
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

void drawKeyRow(int row) {
  canvas->fillRect(0, 0, SCREEN_W, WS_KEY_H, C_BG);
  const std::vector<Key> keys = keyRow(row);
  for (size_t i = 0; i < keys.size(); i++) {
    const Key &k = keys[i];
    const bool pressed = row == pressedRow && static_cast<int>(i) == pressedKey;
    const bool active = (k.code == K_SHIFT && layer == 1);
    canvas->fillRoundRect(k.x + 2, 3, k.w - 4, WS_KEY_H - 6, 4,
                          pressed ? C_BUTTON_PRESS : active ? C_TAB_ACTIVE : C_BUTTON);
    if (k.label) {
      const int16_t tw = uiTextWidth(FONT_LABEL, k.label);
      uiDrawText(FONT_LABEL, k.label, k.x + (k.w - tw) / 2, WS_KEY_H / 2 + 4, k.code == K_JOIN ? C_ACCENT : C_TEXT);
    } else {
      centered(String(static_cast<char>(k.code)), k.x + k.w / 2, WS_KEY_H / 2 - 9, 2, C_TEXT);
    }
  }
  lcdBlit(0, WS_KEYS_Y + row * WS_KEY_H, SCREEN_W, WS_KEY_H);
}

void drawField() {
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  canvas->drawRoundRect(6, 4, SCREEN_W - 12, 36, 4, C_DIM);
  String shown = password + "_";
  while (shown.length() > 1 && uiTextWidth(FONT_BODY, shown.c_str()) > SCREEN_W - 32) shown.remove(0, 1);
  text(shown, 14, 12, 2, C_TEXT);
  lcdBlit(0, WS_FIELD_Y, SCREEN_W, CANVAS_H);
}

void drawKeyboard() {
  for (int r = 0; r < 4; r++) drawKeyRow(r);
}

void showPassword(const char *note = nullptr) {
  screen = S_PASSWORD;
  buttonCount = 0;
  pressedRow = pressedKey = -1;
  lcdFillRect(0, WS_HEADER_H, SCREEN_W, SCREEN_H - WS_HEADER_H, C_BG);
  drawHeader("WiFi password");
  drawTwoLines(WS_INTRO_Y, note ? note : "Password for", display(chosen), note ? C_ACCENT : C_DIM);
  drawField();
  drawKeyboard();
}

// ---------------------------------------------------------------- connecting
void connectTo(const String &ssid, const String &pass) {
  screen = S_CONNECTING;
  lcdFillRect(0, WS_HEADER_H, SCREEN_W, SCREEN_H - WS_HEADER_H, C_BG);
  drawHeader("Album art");
  drawTwoLines(120, "Connecting to", display(ssid));
  setButtons({"Cancel"});
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (pass.isEmpty()) WiFi.begin(ssid.c_str());
  else WiFi.begin(ssid.c_str(), pass.c_str());
  connectStart = millis();
}

void showMessage(const char *a, const String &b, uint32_t ms, Screen next, WifiSetupResult r = WS_RUNNING) {
  screen = S_MESSAGE;
  lcdFillRect(0, WS_HEADER_H, SCREEN_W, SCREEN_H - WS_HEADER_H, C_BG);
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  centered(a, SCREEN_W / 2, 6, 2, C_TEXT);
  centered(fitText(b, 38), SCREEN_W / 2, 30, 1, C_DIM);
  lcdBlit(0, 130, SCREEN_W, CANVAS_H);
  buttonCount = 0;
  messageUntil = millis() + ms;
  afterMessage = next;
  resultAfterMessage = r;
}

void finish(WifiSetupResult r) {
  WiFi.scanDelete();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);  // frees the radio's memory until art downloads need it
  nets.clear();
  nets.shrink_to_fit();
  result = r;
}

void pickNetwork(int r) {
  const uint32_t i = page * WS_ROWS + r;
  if (i >= nets.size()) return;
  chosen = nets[i].ssid;
  password = "";
  layer = 0;
  String s, p;
  usingSaved = wifiSavedCredentials(s, p) && s == chosen;
  if (usingSaved) {
    connectTo(chosen, p);
  } else if (!nets[i].secure) {
    connectTo(chosen, "");
  } else {
    showPassword();
  }
}

void pressKey(const Key &k) {
  switch (k.code) {
    case K_SHIFT: layer = layer == 1 ? 0 : 1; drawKeyboard(); return;
    case K_LAYER: layer = layer >= 2 ? 0 : 2; drawKeyboard(); return;
    case K_SYM2: layer = layer == 2 ? 3 : 2; drawKeyboard(); return;
    case K_DEL:
      if (password.length()) password.remove(password.length() - 1);
      drawField();
      return;
    case K_BACK: showList(); return;
    case K_JOIN:
      if (password.length() >= 8) connectTo(chosen, password);  // WPA minimum
      else drawTwoLines(WS_INTRO_Y, "Passwords are at least 8 characters", display(chosen), C_ACCENT);
      return;
    default:
      if (password.length() < WS_MAX_PASSWORD) password += static_cast<char>(k.code);
      drawField();
      if (layer == 1) {  // shift applies to one letter
        layer = 0;
        drawKeyboard();
      }
      return;
  }
}

}  // namespace

// ---------------------------------------------------------------- public
bool wifiSavedCredentials(String &ssid, String &pass) {
  Preferences prefs;
  if (!prefs.begin("wifi", true)) return false;
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return !ssid.isEmpty();
}

void wifiSetupBegin() {
  result = WS_RUNNING;
  screen = S_LIST;
  startScan();
}

WifiSetupResult wifiSetupTick() {
  if (result != WS_RUNNING) return result;
  if (scanning) {
    const int n = WiFi.scanComplete();
    if (n >= 0 || n == WIFI_SCAN_FAILED) {
      scanning = false;
      if (n > 0) collectScan(n);
      if (screen == S_LIST) showList();
    }
  }
  if (screen == S_CONNECTING) {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      Preferences prefs;
      prefs.begin("wifi", false);
      prefs.putString("ssid", chosen);
      if (!usingSaved) prefs.putString("pass", password);
      prefs.end();
      Serial.printf("[wifi] connected to %s, IP %s, RSSI %d\n", chosen.c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      showMessage("Connected", "Album art will download in the background.", 2000, S_LIST, WS_CONNECTED);
    } else if (millis() - connectStart > WS_CONNECT_TIMEOUT_MS || st == WL_CONNECT_FAILED ||
               st == WL_NO_SSID_AVAIL) {
      Serial.printf("[wifi] connecting to %s failed (status %d)\n", chosen.c_str(), st);
      WiFi.disconnect();
      const bool open = password.isEmpty() && !usingSaved;
      usingSaved = false;
      showMessage("Couldn't connect", open ? "The network didn't answer." : "Check the password and try again.",
                  2500, open ? S_LIST : S_PASSWORD);
    }
  }
  if (screen == S_MESSAGE && static_cast<int32_t>(millis() - messageUntil) >= 0) {
    if (resultAfterMessage != WS_RUNNING) finish(resultAfterMessage);
    else if (afterMessage == S_PASSWORD) showPassword();
    else showList();
  }
  return result;
}

void wifiSetupTouch(TouchPhase phase, int16_t x, int16_t y) {
  if (result != WS_RUNNING) return;
  switch (screen) {
    case S_LIST: {
      if (phase == TOUCH_DOWN) {
        const int b = buttonAt(x, y);
        if (b >= 0) {
          pressedButton = b;
          drawButtons();
        } else if (y >= WS_ROWS_Y && y < WS_STATUS_Y) {
          pressedNet = (y - WS_ROWS_Y) / WS_ROW_H;
          drawNetRow(pressedNet);
        }
      } else if (phase == TOUCH_UP) {
        const int b = pressedButton, r = pressedNet;
        pressedButton = pressedNet = -1;
        if (b >= 0) {
          drawButtons();
          if (buttonAt(x, y) != b) return;
          const char *label = buttonLabels[b];
          if (strcmp(label, "Skip") == 0) {
            showMessage("Skipped", "No album art will be downloaded.", 1500, S_LIST, WS_SKIPPED);
          } else if (strcmp(label, "Rescan") == 0) {
            startScan();
          } else if (strcmp(label, "More") == 0) {
            page = (page + 1) % pageCount();
            for (int i = 0; i < WS_ROWS; i++) drawNetRow(i);
            drawListStatus();
          }
        } else if (r >= 0) {
          drawNetRow(r);
          if (y >= WS_ROWS_Y + r * WS_ROW_H && y < WS_ROWS_Y + (r + 1) * WS_ROW_H) pickNetwork(r);
        }
      }
      return;
    }
    case S_PASSWORD: {
      const int row = y >= WS_KEYS_Y ? (y - WS_KEYS_Y) / WS_KEY_H : -1;
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
          if (oldRow >= 0 && oldRow != pressedRow) drawKeyRow(oldRow);
          if (pressedRow >= 0) drawKeyRow(pressedRow);
        }
      } else {  // TOUCH_UP: type the key under the finger
        const int r = pressedRow, k = pressedKey;
        pressedRow = pressedKey = -1;
        if (r >= 0) {
          drawKeyRow(r);
          if (r == row && k == key) pressKey(keys[k]);
        }
      }
      return;
    }
    case S_CONNECTING:
      if (phase == TOUCH_DOWN && buttonAt(x, y) == 0) {
        pressedButton = 0;
        drawButtons();
      } else if (phase == TOUCH_UP && pressedButton == 0) {
        WiFi.disconnect();
        usingSaved = false;
        if (password.isEmpty()) showList();
        else showPassword();
      }
      return;
    case S_MESSAGE:
      if (phase == TOUCH_UP) messageUntil = millis();  // tap to continue
      return;
  }
}
