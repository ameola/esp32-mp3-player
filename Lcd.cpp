#include "Lcd.h"

#include <SPI.h>
#include <esp_heap_caps.h>
#include <Adafruit_ILI9341.h>

#include "AudioPlayer.h"
#include "Config.h"
#include <driver/gpio.h>

void spiPinDrive() {
  for (int pin : {SPI_SCK, SPI_MOSI, TFT_CS, TFT_DC, SD_CS}) {
    gpio_set_drive_capability(static_cast<gpio_num_t>(pin), static_cast<gpio_drive_cap_t>(SPI_PIN_DRIVE));
  }
}

// Only used for its begin() (pin setup + its init sequence), as in ComponentTest.
static Adafruit_ILI9341 tft(&SPI, TFT_CS, TFT_DC, -1);
GFXcanvas16 *canvas = nullptr;

static void writeLcdCommand(uint8_t command, const uint8_t *data, size_t length) {
  SPI.beginTransaction(SPISettings(LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);
  SPI.transfer(command);
  if (length > 0) {
    digitalWrite(TFT_DC, HIGH);
    for (size_t index = 0; index < length; index++) {
      SPI.transfer(data[index]);
    }
  }
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

// Vendor init table for the LCDwiki ILI9341V module (portrait, BGR, inverted).
static void initVendorIli9341() {
  const uint8_t cf[] = {0x00, 0xC1, 0x30};
  const uint8_t ed[] = {0x64, 0x03, 0x12, 0x81};
  const uint8_t e8[] = {0x85, 0x00, 0x78};
  const uint8_t cb[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
  const uint8_t ea[] = {0x00, 0x00};
  const uint8_t c5[] = {0x1C, 0x35};
  const uint8_t b6[] = {0x0A, 0xA2};
  const uint8_t pixelFormat[] = {0x55};
  const uint8_t f6[] = {0x01, 0x30};
  const uint8_t b1[] = {0x00, 0x1B};
  const uint8_t f2[] = {0x00};
  const uint8_t gammaSelect[] = {0x01};
  const uint8_t gammaPositive[] = {
    0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7,
    0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00
  };
  const uint8_t gammaNegative[] = {
    0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58,
    0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F
  };
  const uint8_t power1[] = {0x13};
  const uint8_t power2[] = {0x13};
  const uint8_t powerAdjustment[] = {0x20};
  const uint8_t vcm2[] = {0xC8};
  const uint8_t memoryAccess[] = {0x08};

  writeLcdCommand(0xCF, cf, sizeof(cf));
  writeLcdCommand(0xED, ed, sizeof(ed));
  writeLcdCommand(0xE8, e8, sizeof(e8));
  writeLcdCommand(0xCB, cb, sizeof(cb));
  writeLcdCommand(0xF7, powerAdjustment, sizeof(powerAdjustment));
  writeLcdCommand(0xEA, ea, sizeof(ea));
  writeLcdCommand(0xC0, power1, sizeof(power1));
  writeLcdCommand(0xC1, power2, sizeof(power2));
  writeLcdCommand(0xC5, c5, sizeof(c5));
  writeLcdCommand(0xC7, vcm2, sizeof(vcm2));
  writeLcdCommand(0x21, nullptr, 0);
  writeLcdCommand(0x36, memoryAccess, sizeof(memoryAccess));
  writeLcdCommand(0xB6, b6, sizeof(b6));
  writeLcdCommand(0x3A, pixelFormat, sizeof(pixelFormat));
  writeLcdCommand(0xF6, f6, sizeof(f6));
  writeLcdCommand(0xB1, b1, sizeof(b1));
  writeLcdCommand(0xF2, f2, sizeof(f2));
  writeLcdCommand(0x26, gammaSelect, sizeof(gammaSelect));
  writeLcdCommand(0xE0, gammaPositive, sizeof(gammaPositive));
  writeLcdCommand(0xE1, gammaNegative, sizeof(gammaNegative));
  writeLcdCommand(0x11, nullptr, 0);
  delay(120);
  writeLcdCommand(0x29, nullptr, 0);
}

static uint8_t backlightPercent = 100;
static bool backlightReady = false;

void lcdSetBacklight(uint8_t percent) {
  backlightPercent = min<uint8_t>(percent, 100);
  if (backlightReady) {
    // Perceived brightness grows roughly with the square of the duty cycle.
    const uint32_t duty = (static_cast<uint32_t>(backlightPercent) * backlightPercent * 255 + 5000) / 10000;
    ledcWrite(TFT_BL, backlightPercent ? max<uint32_t>(duty, 3) : 0);
  }
}

void lcdInit() {
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  // Shared LCD + touch reset.
  pinMode(RESET_SHARED, OUTPUT);
  digitalWrite(RESET_SHARED, HIGH);
  delay(50);
  digitalWrite(RESET_SHARED, LOW);
  delay(100);
  digitalWrite(RESET_SHARED, HIGH);
  delay(500);

  // PWM at 25 kHz: above hearing, so it can't whine through the amp.
  ledcAttach(TFT_BL, 25000, 8);
  gpio_set_drive_capability(static_cast<gpio_num_t>(TFT_BL), static_cast<gpio_drive_cap_t>(BL_PIN_DRIVE));
  backlightReady = true;
  lcdSetBacklight(backlightPercent);

  // Same bring-up order as ComponentTest.
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);
  tft.begin(2000000);
  initVendorIli9341();
  spiPinDrive();

  // In internal RAM (23 KB): every row and label is composed here, and in
  // PSRAM that is markedly slower. Blocks over 4 KB normally go to PSRAM.
  heap_caps_malloc_extmem_enable(32 * 1024);
  canvas = new GFXcanvas16(SCREEN_W, CANVAS_H);
  heap_caps_malloc_extmem_enable(4096);
  canvas->setTextWrap(false);
  lcdFillRect(0, 0, SCREEN_W, SCREEN_H, C_BG);
}

// Clips the rect to the screen and opens the address window (CASET, RASET,
// RAMWR), exactly as ComponentTest's writeLcdRect does. Returns false if empty.
static bool lcdSetWindow(int16_t &x, int16_t &y, int16_t &w, int16_t &h) {
  if (w <= 0 || h <= 0 || x >= SCREEN_W || y >= SCREEN_H || x + w <= 0 || y + h <= 0) {
    return false;
  }
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) { w = SCREEN_W - x; }
  if (y + h > SCREEN_H) { h = SCREEN_H - y; }

  const uint8_t xRange[] = {
    static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
    static_cast<uint8_t>((x + w - 1) >> 8), static_cast<uint8_t>(x + w - 1)
  };
  const uint8_t yRange[] = {
    static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
    static_cast<uint8_t>((y + h - 1) >> 8), static_cast<uint8_t>(y + h - 1)
  };
  writeLcdCommand(0x2A, xRange, sizeof(xRange));
  writeLcdCommand(0x2B, yRange, sizeof(yRange));
  writeLcdCommand(0x2C, nullptr, 0);
  return true;
}

static void lcdBeginData() {
  SPI.beginTransaction(SPISettings(LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, HIGH);
}

static void lcdEndData() {
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

static bool muted = false;

void lcdSetMuted(bool on) { muted = on; }
bool lcdMuted() { return muted; }

void lcdPushPixels(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels) {
  int16_t cx = x, cy = y, cw = w, ch = h;
  if (!lcdSetWindow(cx, cy, cw, ch)) {
    return;
  }
  static uint8_t line[SCREEN_W * 2];
  for (int row = 0; row < ch; row++) {
    const uint16_t *src = pixels + (cy - y + row) * w + (cx - x);
    for (int i = 0; i < cw; i++) {
      line[i * 2] = src[i] >> 8;
      line[i * 2 + 1] = src[i] & 0xFF;
    }
    lcdBeginData();
    SPI.writeBytes(line, cw * 2);
    lcdEndData();
  }
}

void lcdFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (muted || !lcdSetWindow(x, y, w, h)) {
    return;
  }
  static uint8_t line[SCREEN_W * 2];
  for (int i = 0; i < w; i++) {
    line[i * 2] = color >> 8;
    line[i * 2 + 1] = color & 0xFF;
  }
  lcdBeginData();
  for (int row = 0; row < h; row++) {
    SPI.writeBytes(line, w * 2);
  }
  lcdEndData();
}

void lcdBlit(int16_t dstX, int16_t dstY, int16_t w, int16_t h) {
  lcdBlitFrom(0, dstX, dstY, w, h);
}

void lcdBlitFrom(int16_t fromY, int16_t dstX, int16_t dstY, int16_t w, int16_t h) {
  // Never read past the canvas: a band taller than it would send whatever
  // memory follows (garbage on screen).
  h = min<int16_t>(h, canvas->height() - fromY);
  if (h <= 0) return;
  int16_t x = dstX, y = dstY, cw = w, ch = h;
  if (muted || !lcdSetWindow(x, y, cw, ch)) {
    return;
  }
  const uint16_t *pixels = canvas->getBuffer();
  const int stride = canvas->width();
  const int srcX = x - dstX;
  const int srcY = y - dstY + fromY;
  static uint8_t line[SCREEN_W * 2];
  for (int row = 0; row < ch; row++) {
    const uint16_t *src = pixels + (srcY + row) * stride + srcX;
    for (int i = 0; i < cw; i++) {
      line[i * 2] = src[i] >> 8;
      line[i * 2 + 1] = src[i] & 0xFF;
    }
    lcdBeginData();
    SPI.writeBytes(line, cw * 2);
    lcdEndData();  // the bus is free between lines for the audio task's SD reads
  }
}

static int16_t scrollTop = 0;

void lcdScrollArea(int16_t top, int16_t height) {
  scrollTop = top;
  const int16_t bottom = SCREEN_H - top - height;
  const uint8_t def[] = {
    static_cast<uint8_t>(top >> 8), static_cast<uint8_t>(top),
    static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
    static_cast<uint8_t>(bottom >> 8), static_cast<uint8_t>(bottom),
  };
  writeLcdCommand(0x33, def, sizeof(def));  // VSCRDEF
  lcdScrollOffset(0);
}

void lcdScrollOffset(int16_t offset) {
  if (muted) {
    return;
  }
  const int16_t line = scrollTop + offset;
  const uint8_t start[] = {static_cast<uint8_t>(line >> 8), static_cast<uint8_t>(line)};
  writeLcdCommand(0x37, start, sizeof(start));  // VSCRSADD
}
