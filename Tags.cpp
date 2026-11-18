#include "Tags.h"

#include "Text.h"

const char *durationSourceName(DurationSource source) {
  switch (source) {
    case DUR_XING: return "xing";
    case DUR_VBRI: return "vbri";
    case DUR_TLEN: return "tlen";
    case DUR_CBR: return "cbr";
    default: return "none";
  }
}

// ================================================================ Helpers
static uint32_t syncsafe32(const uint8_t *b) {
  return (static_cast<uint32_t>(b[0] & 0x7F) << 21) | ((b[1] & 0x7F) << 14) |
         ((b[2] & 0x7F) << 7) | (b[3] & 0x7F);
}

static uint32_t be32(const uint8_t *b) {
  return (static_cast<uint32_t>(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

// Undo ID3 unsynchronisation (0xFF 0x00 -> 0xFF). Returns the new length.
static size_t removeUnsync(uint8_t *data, size_t n) {
  size_t out = 0;
  for (size_t i = 0; i < n; i++) {
    data[out++] = data[i];
    if (data[i] == 0xFF && i + 1 < n && data[i + 1] == 0x00) {
      i++;
    }
  }
  return out;
}

// ID3v2 text frame body: one encoding byte, then the text.
static String decodeId3Text(const uint8_t *data, size_t n) {
  if (n < 2) {
    return "";
  }
  String text;
  switch (data[0]) {
    case 0: text = decodeLatin1(data + 1, n - 1); break;
    case 1: text = decodeUtf16(data + 1, n - 1, false); break;
    case 2: text = decodeUtf16(data + 1, n - 1, true); break;
    case 3: text = decodeUtf8(data + 1, n - 1); break;
  }
  text.trim();
  return text;
}

// Leading integer of "3", "03/12", "2007-05-01"; 0 if none.
static uint32_t leadingNumber(const String &text) {
  uint32_t value = 0;
  for (size_t i = 0; i < text.length() && isdigit(static_cast<unsigned char>(text[i])); i++) {
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

// ================================================================ ID3v2
enum Field : uint8_t {
  F_TITLE, F_ARTIST, F_ALBUM, F_ALBUM_ARTIST, F_TRACK, F_DISC, F_YEAR, F_LENGTH,
  F_COUNT, F_NONE = 0xFF
};

static Field fieldFor(const char *id, uint8_t ver) {
  static const char *const v22[F_COUNT] = {"TT2", "TP1", "TAL", "TP2", "TRK", "TPA", "TYE", "TLE"};
  static const char *const v23[F_COUNT] = {"TIT2", "TPE1", "TALB", "TPE2", "TRCK", "TPOS", "TYER", "TLEN"};
  const char *const *ids = ver == 2 ? v22 : v23;
  for (uint8_t f = 0; f < F_COUNT; f++) {
    if (strcmp(id, ids[f]) == 0) {
      return static_cast<Field>(f);
    }
  }
  if (ver >= 3 && strcmp(id, "TDRC") == 0) {  // v2.4 recording time (also seen in v2.3)
    return F_YEAR;
  }
  return F_NONE;
}

static bool fieldMissing(const TagInfo &tags, Field f, uint32_t tlenMs) {
  switch (f) {
    case F_TITLE: return tags.title.isEmpty();
    case F_ARTIST: return tags.artist.isEmpty();
    case F_ALBUM: return tags.album.isEmpty();
    case F_ALBUM_ARTIST: return tags.albumArtist.isEmpty();
    case F_TRACK: return tags.track == 0;
    case F_DISC: return tags.disc == 0;
    case F_YEAR: return tags.year == 0;
    case F_LENGTH: return tlenMs == 0;
    default: return false;
  }
}

static void storeField(TagInfo &tags, Field f, const String &text, uint32_t &tlenMs) {
  switch (f) {
    case F_TITLE: tags.title = text; break;
    case F_ARTIST: tags.artist = text; break;
    case F_ALBUM: tags.album = text; break;
    case F_ALBUM_ARTIST: tags.albumArtist = text; break;
    case F_TRACK: tags.track = min<uint32_t>(leadingNumber(text), 0xFFFF); break;
    case F_DISC: tags.disc = min<uint32_t>(leadingNumber(text), 0xFF); break;
    case F_YEAR: tags.year = min<uint32_t>(leadingNumber(text), 0xFFFF); break;
    case F_LENGTH: tlenMs = leadingNumber(text); break;
    default: break;
  }
}

// Parses an ID3v2 tag at offset 0. Returns the offset just past the tag
// (0 if there is none) and the TLEN value in tlenMs.
static uint32_t readId3v2(TagSource &src, TagInfo &tags, uint32_t &tlenMs) {
  uint8_t h[10];
  if (src.readAt(0, h, 10) != 10 || memcmp(h, "ID3", 3) != 0) {
    return 0;
  }
  const uint8_t ver = h[3];
  const uint32_t tagEnd = 10 + syncsafe32(h + 6);
  const uint32_t afterTag = tagEnd + ((h[5] & 0x10) ? 10 : 0);  // footer
  if (ver < 2 || ver > 4) {
    return afterTag;
  }
  const bool tagUnsync = h[5] & 0x80;
  uint32_t pos = 10;
  if ((h[5] & 0x40) && ver >= 3) {  // extended header
    uint8_t e[4];
    if (src.readAt(10, e, 4) != 4) {
      return afterTag;
    }
    pos += ver == 4 ? syncsafe32(e) : be32(e) + 4;
  }

  const uint32_t headerLen = ver == 2 ? 6 : 10;
  uint8_t found = 0;
  while (pos + headerLen <= tagEnd && found < F_COUNT) {
    uint8_t fh[10];
    if (src.readAt(pos, fh, headerLen) != headerLen || fh[0] == 0) {
      break;  // read error or reached padding
    }
    char id[5] = {0};
    uint32_t size;
    uint16_t flags = 0;
    if (ver == 2) {
      memcpy(id, fh, 3);
      size = (static_cast<uint32_t>(fh[3]) << 16) | (fh[4] << 8) | fh[5];
    } else {
      memcpy(id, fh, 4);
      size = ver == 4 ? syncsafe32(fh + 4) : be32(fh + 4);
      flags = (fh[8] << 8) | fh[9];
    }
    pos += headerLen;
    if (size == 0 || pos + size > tagEnd) {
      break;
    }

    const Field field = fieldFor(id, ver);
    const bool compressedOrEncrypted =
      (ver == 3 && (flags & 0x00C0)) || (ver == 4 && (flags & 0x000C));
    if (field != F_NONE && !compressedOrEncrypted && fieldMissing(tags, field, tlenMs)) {
      const uint32_t skip = (ver == 4 && (flags & 0x0001)) ? 4 : 0;  // data length indicator
      uint8_t buf[200];
      const size_t want = min(static_cast<size_t>(size - min(size, skip)), sizeof(buf));
      size_t n = src.readAt(pos + skip, buf, want);
      if (tagUnsync || (ver == 4 && (flags & 0x0002))) {
        n = removeUnsync(buf, n);
      }
      storeField(tags, field, decodeId3Text(buf, n), tlenMs);
      found++;
    }
    pos += size;
  }
  return afterTag;
}

// ================================================================ ID3v1
static String id3v1Field(const uint8_t *s, size_t n) {
  String text = decodeLatin1(s, n);
  text.trim();
  return text;
}

// Returns true if an ID3v1 tag is present; fills only missing fields.
static bool readId3v1(TagSource &src, TagInfo &tags) {
  const uint32_t size = src.size();
  uint8_t tag[128];
  if (size < sizeof(tag) || src.readAt(size - sizeof(tag), tag, sizeof(tag)) != sizeof(tag) ||
      memcmp(tag, "TAG", 3) != 0) {
    return false;
  }
  if (tags.title.isEmpty()) tags.title = id3v1Field(tag + 3, 30);
  if (tags.artist.isEmpty()) tags.artist = id3v1Field(tag + 33, 30);
  if (tags.album.isEmpty()) tags.album = id3v1Field(tag + 63, 30);
  if (tags.year == 0) tags.year = leadingNumber(id3v1Field(tag + 93, 4));
  if (tags.track == 0 && tag[125] == 0) tags.track = tag[126];  // ID3v1.1
  return true;
}

// ================================================================ MPEG frames
struct MpegHeader {
  uint8_t version;  // 1, 2, or 25 (MPEG 2.5)
  uint8_t layer;    // 1..3
  bool mono;
  uint32_t bitrateKbps;
  uint32_t sampleRate;
  uint32_t frameLen;
  uint32_t samplesPerFrame;
};

static bool parseMpegHeader(const uint8_t *h, MpegHeader &m) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
    return false;
  }
  const uint8_t versionBits = (h[1] >> 3) & 3;  // 0: 2.5, 1: reserved, 2: 2, 3: 1
  const uint8_t layerBits = (h[1] >> 1) & 3;    // 1: III, 2: II, 3: I
  const uint8_t bitrateIndex = h[2] >> 4;
  const uint8_t rateIndex = (h[2] >> 2) & 3;
  if (versionBits == 1 || layerBits == 0 || bitrateIndex == 0 || bitrateIndex == 15 ||
      rateIndex == 3) {
    return false;
  }
  static const uint16_t bitrates[2][3][15] = {
    {  // MPEG 1: layer I, II, III
      {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    },
    {  // MPEG 2 / 2.5: layer I, II, III
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    },
  };
  static const uint16_t rates[3] = {44100, 48000, 32000};

  m.version = versionBits == 3 ? 1 : versionBits == 2 ? 2 : 25;
  m.layer = 4 - layerBits;
  m.mono = (h[3] >> 6) == 3;
  m.bitrateKbps = bitrates[m.version == 1 ? 0 : 1][m.layer - 1][bitrateIndex];
  m.sampleRate = rates[rateIndex] >> (m.version == 1 ? 0 : m.version == 2 ? 1 : 2);
  const uint32_t padding = (h[2] >> 1) & 1;
  if (m.layer == 1) {
    m.samplesPerFrame = 384;
    m.frameLen = (12 * m.bitrateKbps * 1000 / m.sampleRate + padding) * 4;
  } else if (m.layer == 2 || m.version == 1) {
    m.samplesPerFrame = 1152;
    m.frameLen = 144 * m.bitrateKbps * 1000 / m.sampleRate + padding;
  } else {
    m.samplesPerFrame = 576;
    m.frameLen = 72 * m.bitrateKbps * 1000 / m.sampleRate + padding;
  }
  return m.frameLen >= 24;
}

static bool sameStream(const MpegHeader &a, const MpegHeader &b) {
  return a.version == b.version && a.layer == b.layer && a.sampleRate == b.sampleRate;
}

#define SYNC_WINDOW 2048
#define SYNC_MAX_SEARCH (16 * 1024)

// Finds the first MPEG frame at or after `from`, confirmed by a second frame
// header right after it. Returns false if none within SYNC_MAX_SEARCH bytes.
static bool findFirstFrame(TagSource &src, uint32_t from, uint32_t &framePos, MpegHeader &m,
                           uint8_t *window, size_t &windowLen, uint32_t &windowPos) {
  const uint32_t fileSize = src.size();
  for (uint32_t base = from; base < from + SYNC_MAX_SEARCH && base + 4 <= fileSize;
       base += SYNC_WINDOW - 4) {
    const size_t n = src.readAt(base, window, SYNC_WINDOW);
    if (n < 4) {
      return false;
    }
    for (size_t i = 0; i + 4 <= n; i++) {
      if (window[i] != 0xFF || !parseMpegHeader(window + i, m)) {
        continue;
      }
      uint8_t next[4];
      const uint32_t nextPos = base + i + m.frameLen;
      const uint8_t *nextHeader = nullptr;
      if (i + m.frameLen + 4 <= n) {
        nextHeader = window + i + m.frameLen;
      } else if (nextPos + 4 <= fileSize && src.readAt(nextPos, next, 4) == 4) {
        nextHeader = next;
      }
      MpegHeader m2;
      if (nextHeader && parseMpegHeader(nextHeader, m2) && sameStream(m, m2)) {
        framePos = base + i;
        windowLen = n;
        windowPos = base;
        return true;
      }
    }
  }
  return false;
}

// Reads the Xing/Info or VBRI frame count from the first frame, if present.
static uint32_t vbrFrameCount(const uint8_t *frame, size_t avail, const MpegHeader &m,
                              DurationSource &source) {
  if (m.layer == 3) {
    const size_t xingOffset = m.version == 1 ? (m.mono ? 21 : 36) : (m.mono ? 13 : 21);
    if (xingOffset + 12 <= avail &&
        (memcmp(frame + xingOffset, "Xing", 4) == 0 || memcmp(frame + xingOffset, "Info", 4) == 0)) {
      const uint32_t flags = be32(frame + xingOffset + 4);
      if (flags & 1) {
        source = DUR_XING;
        return be32(frame + xingOffset + 8);
      }
    }
  }
  if (36 + 18 <= avail && memcmp(frame + 36, "VBRI", 4) == 0) {
    source = DUR_VBRI;
    return be32(frame + 36 + 14);
  }
  return 0;
}

// ================================================================ Entry point
#ifdef RUN_BENCH
uint32_t tagPhaseUs[3];  // ID3v2, ID3v1, MPEG frame scan (Bench profiling)
#define PHASE(i, expr) do { const uint32_t t_ = micros(); expr; tagPhaseUs[i] += micros() - t_; } while (0)
#else
#define PHASE(i, expr) expr
#endif

bool readTags(TagSource &src, TagInfo &tags) {
  uint32_t tlenMs = 0;
  uint32_t afterTag = 0;
  PHASE(0, afterTag = readId3v2(src, tags, tlenMs));
  // ID3v1 sits in the last 128 bytes. Seeking there makes FatFs walk the
  // file's whole cluster chain (~10 ms per song measured), so only look when
  // ID3v2 left the main fields empty. Without it, audioEnd includes those
  // 128 bytes, which shifts a CBR length estimate by ~10 ms at most.
  bool hasV1 = false;
  if (tags.title.isEmpty() || tags.artist.isEmpty() || tags.album.isEmpty()) {
    PHASE(1, hasV1 = readId3v1(src, tags));  // only fills fields ID3v2 lacked
  }
  const uint32_t fileSize = src.size();
  tags.audioEnd = fileSize - (hasV1 ? 128 : 0);

  static uint8_t window[SYNC_WINDOW];
  size_t windowLen = 0;
  uint32_t windowPos = 0;
  uint32_t framePos = 0;
  MpegHeader m;
  bool foundFrame = false;
  PHASE(2, foundFrame = findFirstFrame(src, afterTag, framePos, m, window, windowLen, windowPos));
  if (!foundFrame) {
    tags.durationSource = tlenMs ? DUR_TLEN : DUR_NONE;
    tags.durationMs = tlenMs;
    return false;
  }
  tags.audioStart = framePos;

  // Xing/VBRI headers sit within the first ~60 bytes of the first frame.
  const uint8_t *frame = window + (framePos - windowPos);
  size_t avail = windowLen - (framePos - windowPos);
  uint8_t head[64];
  if (avail < sizeof(head)) {
    avail = src.readAt(framePos, head, sizeof(head));
    frame = head;
  }
  DurationSource source = DUR_NONE;
  const uint32_t frames = vbrFrameCount(frame, avail, m, source);
  if (frames > 0) {
    tags.durationMs = static_cast<uint64_t>(frames) * m.samplesPerFrame * 1000 / m.sampleRate;
    tags.durationSource = source;
  } else if (tlenMs > 0) {
    tags.durationMs = tlenMs;
    tags.durationSource = DUR_TLEN;
  } else {
    const uint32_t audioBytes = tags.audioEnd > framePos ? tags.audioEnd - framePos : 0;
    tags.durationMs = static_cast<uint64_t>(audioBytes) * 8 / m.bitrateKbps;  // kbps = bits/ms
    tags.durationSource = DUR_CBR;
  }
  return true;
}
