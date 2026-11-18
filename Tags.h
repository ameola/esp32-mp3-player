#pragma once
// ID3 tag reader (ID3v2.2 / 2.3 / 2.4, falling back to ID3v1) plus MP3 length
// detection (Xing/Info or VBRI header, then the TLEN tag, then a CBR estimate).

#include <Arduino.h>

// Random-access byte source, so the same parser runs over Arduino's fs::File
// and over FatFs directly (see CardFile.h).
class TagSource {
public:
  virtual ~TagSource() = default;
  virtual uint32_t size() = 0;
  virtual size_t readAt(uint32_t pos, void *buf, size_t len) = 0;
};

enum DurationSource : uint8_t {
  DUR_NONE,  // no MPEG frame found
  DUR_XING,  // Xing/Info frame count (exact)
  DUR_VBRI,  // VBRI frame count (exact)
  DUR_TLEN,  // TLEN tag (as accurate as the tagger)
  DUR_CBR,   // bitrate estimate (exact for CBR, rough for VBR)
};

const char *durationSourceName(DurationSource source);

struct TagInfo {
  String title;        // empty when missing
  String artist;
  String album;
  String albumArtist;  // TPE2
  uint16_t track = 0;  // 0 when missing
  uint8_t disc = 0;
  uint16_t year = 0;
  uint32_t durationMs = 0;
  DurationSource durationSource = DUR_NONE;
  uint32_t audioStart = 0;  // byte offset of the first MPEG frame
  uint32_t audioEnd = 0;    // end of audio data (before an ID3v1 tag)
};

// Fills tags from the source. Returns false if no MPEG audio frame was found.
bool readTags(TagSource &src, TagInfo &tags);
