#pragma once
// Raw-SPI driver for the LCDwiki ILI9341V panel.
//
// Adafruit_ILI9341's drawing calls don't work with this panel, so (like
// ComponentTest) all pixels are sent with raw SPI using the same command
// sequence: CASET / RASET / RAMWR in separate CS cycles, then pixel data.
// UI areas are composed in an off-screen GFXcanvas16 and blitted, so text
// drawing doesn't cost one SPI command per pixel.

#include <Adafruit_GFX.h>

extern GFXcanvas16 *canvas;  // SCREEN_W x CANVAS_H scratch area

// Resets the panel (and touch controller, which shares the reset line),
// runs the vendor init table, turns on the backlight and clears the screen.
// Backlight brightness, 0 (off) to 100 %; may be called before lcdInit().
void lcdSetBacklight(uint8_t percent);
void lcdInit();

void lcdFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// Sends the top-left w x h pixels of the canvas to the screen at (dstX, dstY).
// Releases the bus after every line, so SD reads (audio, library) can
// interleave with long draws.
void lcdBlit(int16_t dstX, int16_t dstY, int16_t w, int16_t h);

// Like lcdBlit, but sends canvas lines fromY .. fromY+h-1 (part of a composed
// region, e.g. a list row cut off at the list's edge).
void lcdBlitFrom(int16_t fromY, int16_t dstX, int16_t dstY, int16_t w, int16_t h);

// Hardware vertical scrolling. Screen lines top .. top+height-1 show frame
// memory rotated by an offset; lines above and below stay fixed. Screen line
// top+i shows memory line top + (i + offset) % height.
void lcdScrollArea(int16_t top, int16_t height);
void lcdScrollOffset(int16_t offset);  // 0 .. height-1

// Sends w x h RGB565 pixels (row by row) to the screen at (x, y), clipped to
// the screen. Ignores lcdSetMuted.
void lcdPushPixels(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

// While muted, lcdFillRect, lcdBlit, lcdBlitFrom and lcdScrollOffset do
// nothing: a full-screen view (the album cover) keeps the player screen's
// own updates (progress, marquees, status) from drawing over it. The view
// unmutes around its own drawing.
void lcdSetMuted(bool muted);
bool lcdMuted();
