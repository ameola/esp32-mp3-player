#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <ESP_I2S.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#define TFT_CS 13
#define TFT_DC 16
#define RESET_SHARED 17
#define TFT_BL 33
#define TOUCH_SDA 21
#define TOUCH_SCL 22
#define TOUCH_ADDRESS 0x38
#define SD_CS 27
#define TFT_SCK 18
#define TFT_MISO 19
#define TFT_MOSI 23
#define I2S_BCLK 25
#define I2S_LRCLK 26
#define I2S_DOUT 32

#define TFT_BL_ON HIGH
#define TFT_BL_OFF LOW

Adafruit_ILI9341 tft(&SPI, TFT_CS, TFT_DC, -1);
I2SClass audioOut;
bool sdReady = false;
bool audioReady = false;

void writeLcdCommand(uint8_t command, const uint8_t *data, size_t length) {
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
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

void initVendorIli9341() {
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

void writeLcdRect(int16_t x, int16_t y, int16_t w, int16_t h,
                  uint16_t color) {
  if (w <= 0 || h <= 0 || x >= 240 || y >= 320 || x + w <= 0 || y + h <= 0) {
    return;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > 240) {
    w = 240 - x;
  }
  if (y + h > 320) {
    h = 320 - y;
  }

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

  const uint8_t highByte = color >> 8;
  const uint8_t lowByte = color & 0xFF;
  const uint32_t pixelCount = static_cast<uint32_t>(w) * h;
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, HIGH);
  for (uint32_t pixel = 0; pixel < pixelCount; pixel++) {
    SPI.transfer(highByte);
    SPI.transfer(lowByte);
  }
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

class LcdGraphics : public Adafruit_GFX {
public:
  LcdGraphics() : Adafruit_GFX(240, 320) {}

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    writeLcdRect(x, y, 1, 1, color);
  }

  void writePixel(int16_t x, int16_t y, uint16_t color) override {
    writeLcdRect(x, y, 1, 1, color);
  }

  void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                     uint16_t color) override {
    writeLcdRect(x, y, w, h, color);
  }

  void writeFastHLine(int16_t x, int16_t y, int16_t w,
                      uint16_t color) override {
    writeLcdRect(x, y, w, 1, color);
  }

  void writeFastVLine(int16_t x, int16_t y, int16_t h,
                      uint16_t color) override {
    writeLcdRect(x, y, 1, h, color);
  }
};

LcdGraphics lcdGraphics;

bool writeTouchRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readTouchRegisters(uint8_t reg, uint8_t *data, uint8_t length) {
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

void initTouch() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(100000);

  const bool reportModeSet = writeTouchRegister(0x01, 0x01);
  const bool pollingModeSet = writeTouchRegister(0xF9, 0x01);

  uint8_t focaltechId = 0;
  uint8_t chipMid = 0;
  uint8_t chipLow = 0;
  uint8_t chipHigh = 0;
  const bool idsRead =
    readTouchRegisters(0xA8, &focaltechId, 1) &&
    readTouchRegisters(0x9F, &chipMid, 1) &&
    readTouchRegisters(0xA0, &chipLow, 1) &&
    readTouchRegisters(0xA3, &chipHigh, 1);

  Serial.print("FT6336 I2C: ");
  Serial.println(idsRead ? "ACK" : "no response");
  Serial.print("Touch report/poll mode writes: ");
  Serial.println(reportModeSet && pollingModeSet ? "OK" : "failed");
  if (idsRead) {
    Serial.print("FT6336 IDs A8/9F/A0/A3: 0x");
    Serial.print(focaltechId, HEX);
    Serial.print(" 0x");
    Serial.print(chipMid, HEX);
    Serial.print(" 0x");
    Serial.print(chipLow, HEX);
    Serial.print(" 0x");
    Serial.println(chipHigh, HEX);
  }
}

void pollTouch() {
  static bool wasTouched = false;
  uint8_t touchStatus = 0;
  if (!readTouchRegisters(0x02, &touchStatus, 1)) {
    return;
  }

  uint8_t touchCount = touchStatus & 0x0F;
  if (touchCount > 2) {
    touchCount = 2;
  }
  if (touchCount == 0) {
    if (wasTouched) {
      Serial.println("Touch released");
      wasTouched = false;
    }
    delay(30);
    return;
  }

  wasTouched = true;
  for (uint8_t point = 0; point < touchCount; point++) {
    uint8_t data[4];
    const uint8_t pointRegister = 0x03 + point * 6;
    if (!readTouchRegisters(pointRegister, data, sizeof(data))) {
      continue;
    }
    const uint16_t x = ((data[0] & 0x0F) << 8) | data[1];
    const uint16_t y = ((data[2] & 0x0F) << 8) | data[3];
    const uint8_t id = data[2] >> 4;
    Serial.print("Touch ");
    Serial.print(id);
    Serial.print(": x=");
    Serial.print(x);
    Serial.print(" y=");
    Serial.println(y);
  }
  delay(30);
}

void listDirectory(File directory, uint8_t depth) {
  while (true) {
    File entry = directory.openNextFile();
    if (!entry) {
      break;
    }
    if (entry.isDirectory()) {
      Serial.print("DIR  ");
      Serial.println(entry.name());
      if (depth > 0) {
        listDirectory(entry, depth - 1);
      }
    } else {
      Serial.print("FILE ");
      Serial.print(entry.name());
      Serial.print("  ");
      Serial.print(entry.size());
      Serial.println(" bytes");
    }
    entry.close();
  }
}

void testSdCard() {
  sdReady = SD.begin(SD_CS, SPI, 4000000);
  if (!sdReady) {
    Serial.println("SD mount failed; check card, SD_CS, and shared SPI wiring.");
    return;
  }

  Serial.print("SD mounted, size: ");
  Serial.print(SD.cardSize() / (1024ULL * 1024ULL));
  Serial.println(" MiB");
  File root = SD.open("/");
  if (root) {
    listDirectory(root, 1);
    root.close();
  }
}

void playTestTone(uint16_t frequency, uint16_t durationMs) {
  constexpr uint32_t sampleRate = 44100;
  constexpr size_t framesPerBlock = 256;
  int16_t samples[framesPerBlock * 2];
  const uint32_t totalFrames = sampleRate * durationMs / 1000;
  uint32_t frame = 0;

  while (frame < totalFrames) {
    const size_t frames = min(static_cast<size_t>(framesPerBlock),
                              static_cast<size_t>(totalFrames - frame));
    for (size_t index = 0; index < frames; index++) {
      const float phase = 2.0f * PI * frequency * (frame + index) / sampleRate;
      const int16_t sample = static_cast<int16_t>(3500.0f * sinf(phase));
      samples[index * 2] = sample;
      samples[index * 2 + 1] = sample;
    }
    const size_t bytes = audioOut.write(samples, frames * 2 * sizeof(int16_t));
    if (bytes == 0) {
      Serial.println("I2S write failed.");
      return;
    }
    frame += bytes / (2 * sizeof(int16_t));
  }
}

void testAudioAmp() {
  audioOut.setPins(I2S_BCLK, I2S_LRCLK, I2S_DOUT);
  audioReady = audioOut.begin(I2S_MODE_STD, 44100,
                              I2S_DATA_BIT_WIDTH_16BIT,
                              I2S_SLOT_MODE_STEREO);
  if (!audioReady) {
    Serial.println("I2S init failed; check ESP32 core and GPIO wiring.");
    return;
  }

  Serial.println("MAX98357A test: listen for two quiet tones.");
  playTestTone(440, 300);
  delay(100);
  playTestTone(660, 300);
  audioOut.end();
  audioReady = false;
  Serial.println("I2S tone test complete.");
}

void drawTestImage(Adafruit_GFX &canvas) {
  const uint16_t sky = 0x3D1F;
  const uint16_t farMountain = 0x7B6D;
  const uint16_t nearMountain = 0x4A49;
  const uint16_t snow = 0xEF7D;
  const uint16_t ground = 0x2D45;
  const uint16_t water = 0x1D7F;

  canvas.fillScreen(ILI9341_BLACK);
  canvas.fillRect(0, 0, 240, 48, 0x18E3);
  canvas.setTextColor(ILI9341_WHITE);
  canvas.setTextSize(2);
  canvas.setCursor(9, 8);
  canvas.print("LCD BRING-UP");
  canvas.setTextSize(1);
  canvas.setCursor(10, 32);
  canvas.print("ILI9341  |  240 x 320");

  canvas.fillRect(0, 50, 240, 170, sky);
  canvas.fillCircle(187, 91, 23, 0xFFE0);

  canvas.fillTriangle(0, 211, 69, 92, 143, 211, farMountain);
  canvas.fillTriangle(42, 211, 139, 111, 240, 211, nearMountain);
  canvas.fillTriangle(48, 128, 69, 92, 91, 128, snow);
  canvas.fillTriangle(119, 132, 139, 111, 162, 134, snow);

  canvas.fillRect(0, 211, 240, 9, ground);
  canvas.fillRect(0, 220, 240, 36, water);
  canvas.fillTriangle(0, 220, 240, 220, 240, 236, ground);
  canvas.fillTriangle(0, 220, 0, 237, 240, 220, ground);
  canvas.drawFastHLine(18, 238, 72, 0x7DFF);
  canvas.drawFastHLine(139, 232, 56, 0x7DFF);
  canvas.drawFastHLine(105, 247, 91, 0x7DFF);

  canvas.fillRect(0, 260, 60, 60, ILI9341_RED);
  canvas.fillRect(60, 260, 60, 60, ILI9341_GREEN);
  canvas.fillRect(120, 260, 60, 60, ILI9341_BLUE);
  canvas.fillRect(180, 260, 60, 60, ILI9341_WHITE);
  canvas.setTextColor(ILI9341_BLACK);
  canvas.setTextSize(1);
  canvas.setCursor(21, 286);
  canvas.print("RED");
  canvas.setCursor(77, 286);
  canvas.print("GREEN");
  canvas.setCursor(139, 286);
  canvas.print("BLUE");
  canvas.setCursor(193, 286);
  canvas.print("WHITE");

  canvas.drawRect(0, 0, 240, 320, ILI9341_WHITE);
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BL_OFF);

  pinMode(RESET_SHARED, OUTPUT);
  digitalWrite(RESET_SHARED, HIGH);
  delay(50);
  digitalWrite(RESET_SHARED, LOW);
  delay(100);
  digitalWrite(RESET_SHARED, HIGH);
  delay(500);

  digitalWrite(TFT_BL, TFT_BL_ON);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin(2000000);
  initVendorIli9341();
  drawTestImage(lcdGraphics);
  initTouch();
  Serial.println("Display ready; tap the panel to verify touch coordinates.");
  testSdCard();
  testAudioAmp();
  Serial.println("Component test complete.");
}

void loop() {
  pollTouch();
}