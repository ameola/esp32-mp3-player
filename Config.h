#pragma once
// Pins, tuning knobs, screen layout and colors shared by all modules.

#include <Arduino.h>

#include "UiTheme.h"

// ---------------------------------------------------------------- Pins
#define TFT_CS 13
#define TFT_DC 4         // WROVER: GPIO16/17 belong to PSRAM
#define RESET_SHARED 14
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
// Drive strength of the SPI pins the ESP32 drives (SCK, MOSI, the CSs, DC).
// At the default (~20 mA) their sharp edges coupled into the amp's I2S wires
// on the hand-wired WROVER board, and the MAX98357A stuttered while the
// screen drew; the weakest setting (~5 mA) cured most of it, and card
// reads at 20 MHz were byte-identical. The backlight's PWM pin is weakened
// the same way, and the I2S pins to the amp get the strongest drive (~40 mA)
// so noise coupled onto them moves them less: that pair sounded best in a
// listening test. 0-3 = GPIO_DRIVE_CAP_0..3.
#define SPI_PIN_DRIVE 0
#define BL_PIN_DRIVE 0
#define I2S_PIN_DRIVE 3

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
#define FIRMWARE_VERSION "0.3"

#define ART_USER_AGENT "MP3Player-ESP32/0.1 ( https://github.com/ameola )"

#define VOLUME_STEPS 10
#define DEFAULT_VOLUME 4
#define GAIN_PER_STEP 0.08f

// ---------------------------------------------------------------- Layout
#define SCREEN_W 240
#define SCREEN_H 320
// Header: back button or screen title, status icons.
#define HEADER_H 32
// The list, with the A-Z letter strip on its right. Rows follow the density
// and text-size settings (uiRowH(): 34-46 px), so the last one on screen may
// be cut off by the list's end.
#define LIST_Y HEADER_H
#define TABBAR_H 38
#define TABBAR_Y (SCREEN_H - TABBAR_H)                    // 282: Songs, Artists, Albums, Folders, Settings
#define MINI_H 42
#define MINI_Y (TABBAR_Y - MINI_H)                        // 240: mini player
#define LIST_END MINI_Y
#define LIST_AREA_H (LIST_END - LIST_Y)                   // 208
#define ROW_H (uiRowH())
#define FULL_ROWS (LIST_AREA_H / ROW_H)                   // rows wholly on screen
#define VISIBLE_ROWS ((LIST_AREA_H + ROW_H - 1) / ROW_H)  // with the cut one
#define STRIP_W 14
#define LIST_W (SCREEN_W - STRIP_W)                       // 226
#define STRIP_PITCH 7                                     // 27 letters x 7 px = 189 px
#define STRIP_TOP 10
#define TAB_COUNT 5
#define TAB_W (SCREEN_W / TAB_COUNT)                      // 48
#define CANVAS_H 48  // tallest region composed at once (a Large-text row: 46)

// ---------------------------------------------------------------- Colors (RGB565)
// From the current theme (UiTheme.h, Settings > Display).
#define C_BG (pal.bg)
#define C_HEADER (pal.surface)
#define C_ROW_ALT (pal.bg)
#define C_ROW_PLAYING (pal.playing)
#define C_TEXT (pal.text)
#define C_DIM (pal.dim)
#define C_ACCENT (pal.accent)
#define C_BUTTON (pal.surface2)
#define C_BUTTON_PRESS (pal.line)
#define C_PROGRESS_BG (pal.line)
#define C_TAB_ACTIVE (pal.surface2)
