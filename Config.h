#pragma once
// Pins, tuning knobs, screen layout and colors shared by all modules.

#include <Arduino.h>

// ---------------------------------------------------------------- Pins
#define TFT_CS 13
#define TFT_DC 16
#define RESET_SHARED 17
#define TFT_BL 33
#define TOUCH_SDA 21
#define TOUCH_SCL 22
#define TOUCH_ADDRESS 0x38
#define SD_CS 27
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23
#define I2S_BCLK 25
#define I2S_LRCLK 26
#define I2S_DOUT 32

// ---------------------------------------------------------------- Tuning
// LCD: ComponentTest proved 2 MHz; 8 and 16 MHz were then verified clean on
// screen (library screen 800 -> 322 -> 242 ms after display init). Drop
// back to 8000000 if the screen ever shows streaks, wrong colors or garbage.
// The SD card has its own clock setting on the shared bus; 20 MHz passed the
// Bench CRC/write checks (including reads interleaved with LCD draws).
#define LCD_SPI_HZ 16000000
#define SD_SPI_HZ 20000000

// If taps land in the wrong place, flip these. Serial prints raw and
// mapped touch coordinates to help.
#define TOUCH_SWAP_XY false
#define TOUCH_INVERT_X false
#define TOUCH_INVERT_Y false

// Album art downloads: MusicBrainz asks every program to identify itself.
#define ART_USER_AGENT "MP3Player-ESP32/0.1 ( https://github.com/ameola )"

#define VOLUME_STEPS 10
#define DEFAULT_VOLUME 4
#define GAIN_PER_STEP 0.08f

// ---------------------------------------------------------------- Layout
#define SCREEN_W 240
#define SCREEN_H 320
// Tab bar: Songs / Artists / Albums / Folders, then library status.
#define TAB_H 24
#define TAB_W 48
#define TAB_COUNT 4
#define STATUS_X (TAB_W * TAB_COUNT)              // 192
// Navigation bar: back button, trail, volume.
#define NAV_Y TAB_H
#define NAV_H 22
#define HEADER_H (TAB_H + NAV_H)                  // 46
#define BACK_W 26
#define VOL_DOWN_X 164
#define VOL_TEXT_X 190
#define VOL_UP_X 212
#define VOL_BTN_W 26
// List with the A-Z letter strip on its right.
#define LIST_Y HEADER_H
#define ROW_H 32
#define VISIBLE_ROWS 6
#define LIST_END (LIST_Y + ROW_H * VISIBLE_ROWS)  // 238
#define STRIP_W 20
#define LIST_W (SCREEN_W - STRIP_W)               // 220
#define STRIP_PITCH 7                             // 27 letters x 7 px = 189 px
// Now playing and controls.
#define NOW_Y LIST_END                            // 238
#define CTRL_Y 280
#define CTRL_H 40
#define NOW_H (CTRL_Y - NOW_Y)                    // 42
#define CTRL_BUTTONS 5
#define CTRL_W (SCREEN_W / CTRL_BUTTONS)
#define PROGRESS_X 8
#define PROGRESS_Y (NOW_Y + 34)
#define PROGRESS_W (SCREEN_W - 16)
#define PROGRESS_H 5
#define CANVAS_H 44  // tallest UI region

// ---------------------------------------------------------------- Colors (RGB565)
#define C_BG 0x0000
#define C_HEADER 0x18E3
#define C_ROW_ALT 0x0841
#define C_ROW_PLAYING 0x0B2C
#define C_TEXT 0xFFFF
#define C_DIM 0x8410
#define C_ACCENT 0x07FF
#define C_BUTTON 0x2945
#define C_BUTTON_PRESS 0x4A69
#define C_PROGRESS_BG 0x31A6
#define C_TAB_ACTIVE 0x2B4F
