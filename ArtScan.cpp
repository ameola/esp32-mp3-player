// Album art survey (serial command "artscan"). Read-only: walks the card like
// the indexer, looks for pictures in each song's ID3v2 tag (APIC / PIC) and
// for image files in each folder that holds songs, and prints what decoding
// art on this device would face: formats, JPEG flavours, sizes.
// Runs in its own low-priority task on core 1, so audio and the UI carry on.

#include <Arduino.h>
#include "ArtScan.h"
#include "CardFile.h"
#include "Library.h"

namespace {

enum Kind : uint8_t {
  K_JPEG_BASE,   // baseline / extended sequential: ROM decoder handles it
  K_JPEG_PROG,   // progressive: ROM decoder can't
  K_JPEG_OTHER,  // arithmetic / lossless / no SOF found
  K_PNG,
  K_PNG_INTERLACED,
  K_UNSYNC,      // tag-level unsynchronisation: bytes altered, not examined
  K_OTHER,       // GIF, BMP, a URL ("-->"), unknown
  K_COUNT
};
const char *const kindNames[K_COUNT] = {
  "JPEG baseline", "JPEG progressive", "JPEG other (arith/lossless/bad)", "PNG",
  "PNG interlaced", "unsynchronised (not examined)", "other/unknown"};

struct Img {
  Kind kind = K_OTHER;
  uint16_t w = 0, h = 0;
  uint8_t comps = 0;  // JPEG components: 1 grey, 3 colour, 4 CMYK
};

const uint32_t dimEdges[] = {150, 300, 500, 600, 800, 1000, 1500};
const uint32_t byteEdges[] = {10, 30, 60, 100, 200, 500, 1000};  // KB
#define DIM_BUCKETS (sizeof(dimEdges) / sizeof(dimEdges[0]) + 1)
#define BYTE_BUCKETS (sizeof(byteEdges) / sizeof(byteEdges[0]) + 1)

struct Stats {
  uint32_t kinds[K_COUNT] = {0};
  uint32_t dims[DIM_BUCKETS] = {0};
  uint32_t bytes[BYTE_BUCKETS] = {0};
  uint32_t cmyk = 0, grey = 0;
  uint32_t largestBytes = 0;
  uint64_t totalBytes = 0;
  uint32_t count = 0;

  void add(const Img &img, uint32_t len) {
    count++;
    kinds[img.kind]++;
    totalBytes += len;
    largestBytes = max(largestBytes, len);
    size_t b = 0;
    while (b < BYTE_BUCKETS - 1 && len > byteEdges[b] * 1024) b++;
    bytes[b]++;
    if (img.w && img.h) {
      const uint32_t d = max(img.w, img.h);
      size_t i = 0;
      while (i < DIM_BUCKETS - 1 && d > dimEdges[i]) i++;
      dims[i]++;
    }
    if (img.comps == 4) cmyk++;
    if (img.comps == 1) grey++;
  }
};

struct Scan {
  CardFile file;
  uint32_t songs = 0, withId3v2 = 0, withArt = 0, multiPic = 0, notFront = 0, compressed = 0;
  Stats embedded, folderImgs;
  uint32_t songFolders = 0, foldersWithImage = 0, foldersAllArt = 0, foldersNoArt = 0;
  uint32_t foldersSameArt = 0, foldersMixedArt = 0;
  uint32_t nameFolder = 0, nameCover = 0, nameFront = 0, nameAlbumArt = 0, nameOther = 0;
  // A few paths per awkward kind, to look at on a computer.
  String examples[K_COUNT][3];
  String cmykExample;
  uint32_t start = 0;
};

Scan *scan = nullptr;
TaskHandle_t task = nullptr;

uint16_t be16(const uint8_t *b) { return (b[0] << 8) | b[1]; }
uint32_t be32(const uint8_t *b) {
  return (static_cast<uint32_t>(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
uint32_t syncsafe(const uint8_t *b) {
  return ((b[0] & 0x7F) << 21) | ((b[1] & 0x7F) << 14) | ((b[2] & 0x7F) << 7) | (b[3] & 0x7F);
}

// Examines an image stored at [pos, pos+len) of the file.
Img examine(TagSource &src, uint32_t pos, uint32_t len) {
  Img img;
  uint8_t b[32];
  if (len < 8 || src.readAt(pos, b, 8) != 8) {
    return img;
  }
  if (memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) {
    if (len >= 29 && src.readAt(pos + 8, b, 21) == 21 && memcmp(b + 4, "IHDR", 4) == 0) {
      img.w = min<uint32_t>(be32(b + 8), 0xFFFF);
      img.h = min<uint32_t>(be32(b + 12), 0xFFFF);
      img.kind = b[20] ? K_PNG_INTERLACED : K_PNG;
    } else {
      img.kind = K_PNG;
    }
    return img;
  }
  if (b[0] != 0xFF || b[1] != 0xD8) {
    return img;
  }
  img.kind = K_JPEG_OTHER;
  uint32_t p = 2;
  for (int guard = 0; guard < 200 && p + 4 <= len; guard++) {
    if (src.readAt(pos + p, b, 4) != 4 || b[0] != 0xFF) {
      return img;
    }
    const uint8_t m = b[1];
    if (m == 0xFF) {  // fill byte
      p++;
      continue;
    }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {
      p += 2;
      continue;
    }
    if (m == 0xD9 || m == 0xDA) {  // end of image / start of scan before any SOF
      return img;
    }
    const uint16_t segLen = be16(b + 2);
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (src.readAt(pos + p + 4, b, 6) == 6) {
        img.h = be16(b + 1);
        img.w = be16(b + 3);
        img.comps = b[5];
      }
      img.kind = (m == 0xC0 || m == 0xC1) ? K_JPEG_BASE
                 : (m == 0xC2)            ? K_JPEG_PROG
                                          : K_JPEG_OTHER;
      return img;
    }
    p += 2 + segLen;
  }
  return img;
}

void noteExample(Kind kind, const String &path) {
  if (kind == K_JPEG_BASE || kind == K_PNG) return;
  for (auto &e : scan->examples[kind]) {
    if (e.isEmpty()) {
      e = path;
      return;
    }
  }
}

// Embedded picture of one song: returns its byte length (0 = none).
uint32_t songArt(const String &path) {
  TagSource &src = scan->file;
  uint8_t h[10];
  if (src.readAt(0, h, 10) != 10 || memcmp(h, "ID3", 3) != 0) {
    return 0;
  }
  scan->withId3v2++;
  const uint8_t ver = h[3];
  if (ver < 2 || ver > 4) return 0;
  const uint32_t tagEnd = 10 + syncsafe(h + 6);
  const bool tagUnsync = h[5] & 0x80;
  uint32_t pos = 10;
  if ((h[5] & 0x40) && ver >= 3) {
    uint8_t e[4];
    if (src.readAt(10, e, 4) != 4) return 0;
    pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
  }
  const uint32_t headerLen = ver == 2 ? 6 : 10;
  uint32_t pictures = 0;
  bool haveChoice = false, choiceFront = false, choiceUnsync = false;
  uint32_t choicePos = 0, choiceLen = 0;

  while (pos + headerLen <= tagEnd) {
    uint8_t fh[10];
    if (src.readAt(pos, fh, headerLen) != headerLen || fh[0] == 0) break;
    uint32_t size;
    uint16_t flags = 0;
    bool isPic;
    if (ver == 2) {
      size = (static_cast<uint32_t>(fh[3]) << 16) | (fh[4] << 8) | fh[5];
      isPic = memcmp(fh, "PIC", 3) == 0;
    } else {
      size = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
      flags = (fh[8] << 8) | fh[9];
      isPic = memcmp(fh, "APIC", 4) == 0;
    }
    pos += headerLen;
    if (size == 0 || pos + size > tagEnd) break;
    if (isPic) {
      pictures++;
      if ((ver == 3 && (flags & 0x00C0)) || (ver == 4 && (flags & 0x000C))) {
        scan->compressed++;
      } else {
        const uint32_t skip = (ver == 4 && (flags & 0x0001)) ? 4 : 0;
        uint8_t b[160];
        const size_t n = src.readAt(pos + skip, b, min<uint32_t>(size - min(size, skip), sizeof(b)));
        // encoding, then MIME (v2.3+) or 3-char format (v2.2), type, description
        size_t i = 1;
        if (ver == 2) {
          i = 4;
        } else {
          while (i < n && b[i]) i++;
          i++;
        }
        const uint8_t type = i < n ? b[i] : 0;
        i++;
        const bool wide = n && (b[0] == 1 || b[0] == 2);
        if (wide) {
          while (i + 1 < n && (b[i] || b[i + 1])) i += 2;
          i += 2;
        } else {
          while (i < n && b[i]) i++;
          i++;
        }
        if (i <= n && (!haveChoice || (type == 3 && !choiceFront))) {
          haveChoice = true;
          choiceFront = type == 3;
          choicePos = pos + skip + i;
          choiceLen = size - skip - i;
          choiceUnsync = tagUnsync || (ver == 4 && (flags & 0x0002));
        }
      }
    }
    pos += size;
  }
  if (pictures > 1) scan->multiPic++;
  if (!haveChoice) return 0;
  scan->withArt++;
  if (!choiceFront) scan->notFront++;
  Img img;
  if (choiceUnsync) {
    img.kind = K_UNSYNC;
  } else {
    img = examine(src, choicePos, choiceLen);
  }
  scan->embedded.add(img, choiceLen);
  noteExample(img.kind, path);
  if (img.comps == 4 && scan->cmykExample.isEmpty()) scan->cmykExample = path;
  return choiceLen;
}

bool isImageName(const char *name) {
  if (name[0] == '.') return false;
  const size_t len = strlen(name);
  return (len > 4 && (strcasecmp(name + len - 4, ".jpg") == 0 || strcasecmp(name + len - 4, ".png") == 0)) ||
         (len > 5 && strcasecmp(name + len - 5, ".jpeg") == 0);
}

// Lower is preferred.
int imageRank(const char *name) {
  if (strncasecmp(name, "cover.", 6) == 0) return 0;
  if (strncasecmp(name, "folder.", 7) == 0) return 1;
  if (strncasecmp(name, "front.", 6) == 0) return 2;
  if (strncasecmp(name, "albumart", 8) == 0) return 3;
  return 4;
}

void walk(const String &path, uint32_t cluster, int depth) {
  CardDir dir;
  const String fatfsPath = cardPath(path.isEmpty() ? "/" : path);
  if (!dir.open(fatfsPath.c_str(), cluster)) {
    Serial.printf("[art] cannot open %s\n", path.c_str());
    return;
  }
  FILINFO info;
  uint32_t songsHere = 0, artHere = 0, firstArtLen = 0;
  bool sameArt = true;
  String bestImage;
  int bestRank = 99;
  while (dir.read(info)) {
    if (info.fattrib & (AM_HID | AM_SYS)) continue;
    const String child = path + "/" + info.fname;
    if (info.fattrib & AM_DIR) {
      if (depth < 12 && !librarySkipFolder(info.fname)) {
        uint32_t sub = 0;
        cardCaptureDir(dir.dir(), info, sub);
        walk(child, sub, depth + 1);
      }
    } else if (libraryIsMp3Name(info.fname)) {
      uint32_t fc = 0;
      const bool captured = cardCaptureEntry(dir.dir(), info, fc);
      if (!(captured && scan->file.openCluster(fc, info.fsize)) && !scan->file.open(child)) {
        continue;
      }
      songsHere++;
      scan->songs++;
      const uint32_t len = songArt(child);
      if (len) {
        if (artHere == 0) firstArtLen = len;
        else if (len != firstArtLen) sameArt = false;
        artHere++;
      }
      scan->file.close();
      if (scan->songs % 2000 == 0) {
        Serial.printf("[art] %lu songs, %lu with art, %lu s\n", scan->songs, scan->withArt,
                      (millis() - scan->start) / 1000);
      }
      if (scan->songs % 25 == 0) vTaskDelay(1);
    } else if (isImageName(info.fname)) {
      const int r = imageRank(info.fname);
      if (r < bestRank) {
        bestRank = r;
        bestImage = child;
      }
    }
  }
  dir.close();
  if (!songsHere) return;
  scan->songFolders++;
  if (artHere == songsHere) scan->foldersAllArt++;
  if (artHere == 0) scan->foldersNoArt++;
  if (artHere > 1) (sameArt ? scan->foldersSameArt : scan->foldersMixedArt)++;
  if (!bestImage.isEmpty()) {
    scan->foldersWithImage++;
    uint32_t *names[] = {&scan->nameCover, &scan->nameFolder, &scan->nameFront,
                         &scan->nameAlbumArt, &scan->nameOther};
    (*names[bestRank])++;
    if (scan->file.open(bestImage)) {
      const Img img = examine(scan->file, 0, scan->file.size());
      scan->folderImgs.add(img, scan->file.size());
      scan->file.close();
    }
  }
}

void printStats(const char *title, const Stats &s) {
  if (!s.count) return;
  Serial.printf("[art] %s: %lu images, average %lu KB, largest %lu KB\n", title, s.count,
                static_cast<uint32_t>(s.totalBytes / s.count / 1024), s.largestBytes / 1024);
  for (int k = 0; k < K_COUNT; k++) {
    if (s.kinds[k]) {
      Serial.printf("[art]   %-32s %6lu (%lu%%)\n", kindNames[k], s.kinds[k], s.kinds[k] * 100 / s.count);
    }
  }
  if (s.cmyk || s.grey) {
    Serial.printf("[art]   JPEG CMYK %lu, greyscale %lu\n", s.cmyk, s.grey);
  }
  Serial.print("[art]   largest side:");
  for (size_t i = 0; i < DIM_BUCKETS; i++) {
    if (i < DIM_BUCKETS - 1) Serial.printf(" <=%lu:%lu", dimEdges[i], s.dims[i]);
    else Serial.printf(" >%lu:%lu", dimEdges[i - 1], s.dims[i]);
  }
  Serial.print("\n[art]   bytes (KB):");
  for (size_t i = 0; i < BYTE_BUCKETS; i++) {
    if (i < BYTE_BUCKETS - 1) Serial.printf(" <=%lu:%lu", byteEdges[i], s.bytes[i]);
    else Serial.printf(" >%lu:%lu", byteEdges[i - 1], s.bytes[i]);
  }
  Serial.println();
}

void report() {
  Scan &s = *scan;
  Serial.printf("[art] ==== %lu songs in %lu folders, scanned in %lu s\n", s.songs, s.songFolders,
                (millis() - s.start) / 1000);
  Serial.printf("[art] songs with ID3v2 %lu, with a picture %lu (%lu%%); several pictures %lu, "
                "no front-cover type %lu, compressed frames %lu\n",
                s.withId3v2, s.withArt, s.songs ? s.withArt * 100 / s.songs : 0, s.multiPic,
                s.notFront, s.compressed);
  Serial.printf("[art] folders: every song has art %lu, no song has art %lu; "
                "same picture size in all songs %lu, differing %lu\n",
                s.foldersAllArt, s.foldersNoArt, s.foldersSameArt, s.foldersMixedArt);
  Serial.printf("[art] folders with an image file %lu (cover %lu, folder %lu, front %lu, "
                "AlbumArt* %lu, other %lu)\n",
                s.foldersWithImage, s.nameCover, s.nameFolder, s.nameFront, s.nameAlbumArt, s.nameOther);
  printStats("embedded", s.embedded);
  printStats("folder images", s.folderImgs);
  for (int k = 0; k < K_COUNT; k++) {
    for (const auto &e : s.examples[k]) {
      if (!e.isEmpty()) Serial.printf("[art] e.g. %s: %s\n", kindNames[k], e.c_str());
    }
  }
  if (!s.cmykExample.isEmpty()) Serial.printf("[art] e.g. CMYK: %s\n", s.cmykExample.c_str());
  Serial.printf("[art] done; stack left %u bytes\n", uxTaskGetStackHighWaterMark(nullptr));
}

void scanTask(void *) {
  scan->start = millis();
  scan->file.setCache(2048);
  walk("", 0, 0);
  report();
  delete scan;
  scan = nullptr;
  task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

void artScanStart() {
  if (task) {
    Serial.println("[art] already running");
    return;
  }
  scan = new (std::nothrow) Scan();
  if (!scan || xTaskCreatePinnedToCore(scanTask, "artscan", 8192, nullptr, 1, &task, 1) != pdPASS) {
    Serial.println("[art] not enough memory");
    delete scan;
    scan = nullptr;
    task = nullptr;
  }
}

const char *artDescribeImage(TagSource &src, uint32_t pos, uint32_t len, uint16_t &w, uint16_t &h) {
  const Img img = examine(src, pos, len);
  w = img.w;
  h = img.h;
  return kindNames[img.kind];
}
