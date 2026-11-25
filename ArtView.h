#pragma once
// Draws album covers (baseline JPEG) from the card with the ESP32 ROM's
// TJpgDec decoder, straight to the LCD one block at a time: about 4 KB of
// RAM while drawing, nothing after.
#include <Arduino.h>

struct ArtDrawResult {
  bool ok = false;
  uint16_t width = 0, height = 0;  // of the image file
  uint8_t scale = 0;               // drawn at 1/(1 << scale)
  uint32_t bytes = 0;
  uint32_t ms = 0;
  const char *error = "";          // when !ok
};

// Draws the JPEG at cardPath (FatFs path, e.g. from cardPath()) centered in
// the box, scaled down by 1/2, 1/4 or 1/8 if it's much bigger and cropped to
// the box. The box's background is left as it is.
ArtDrawResult artDrawJpeg(const String &cardPath, int16_t boxX, int16_t boxY, int16_t boxW, int16_t boxH);

// Thumbnails for lists: ART_THUMB x ART_THUMB RGB565 pixels, kept in the
// ThumbStore. Before it, each was a file next to its cover, "<key>.thm"
// (read once to move it into the store).
#define ART_THUMB 30
#define ART_THUMB_BYTES (ART_THUMB * ART_THUMB * 2)

String artThumbPath(const String &coverPath);  // ".jpg" -> ".thm"
// Makes the thumbnail of a JPEG (FatFs path): TJpgDec at up to 1/8 scale
// and a box filter, ~100-150 ms for 250 px; progressive files (TJpgDec
// can't read them) from their DC scan.
bool artMakeThumb(const String &jpgPath, uint16_t *pixels);
bool artReadThumb(const String &thumbPath, uint16_t *pixels);  // an old .thm file
