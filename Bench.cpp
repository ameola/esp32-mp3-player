#ifdef RUN_BENCH

#include "Bench.h"

#include <SD.h>
#include <SPI.h>
#include <esp_rom_crc.h>
#include "ff.h"

#include "Config.h"
#include "Lcd.h"
#include "CardFile.h"
#include "LibStore.h"
#include "Library.h"
#include "Tags.h"
#include <vector>
#include "Ui.h"

#define BENCH_ROOT "/_bench"
#define BENCH_LIB "/_bench/lib"
#define BENCH_DIRS 20
#define BENCH_FILES_PER_DIR 100
#define BENCH_ART_BYTES 16384   // fake cover art placed before the text frames
#define BENCH_AUDIO_BYTES 4096
#define REAL_FILE "/mp3/0001.mp3"
#define READ_BYTES (1024 * 1024)
#define WRITE_BYTES (512 * 1024)
#define FATFS_DRIVE "0:"

static uint8_t buf[4096];

static bool mountAt(uint32_t hz) {
  SD.end();
  return SD.begin(SD_CS, SPI, hz);
}

static void status(const char *line1, const String &line2) {
  Serial.printf("[bench] %s %s\n", line1, line2.c_str());
  uiShowMessage(line1, line2.c_str());
}

// ---------------------------------------------------------------- Raw speed
struct ReadResult {
  bool ok;
  float kbps;
  uint32_t crc;
};

// Reads the first READ_BYTES of the real MP3. With interleaveLcd, draws a
// strip on the LCD every 16 KB to check the shared bus switches cleanly.
static ReadResult readTest(bool interleaveLcd) {
  File f = SD.open(REAL_FILE);
  if (!f) {
    return {false, 0, 0};
  }
  uint32_t crc = 0;
  size_t total = 0;
  const uint32_t t0 = micros();
  while (total < READ_BYTES) {
    const size_t n = f.read(buf, min(sizeof(buf), static_cast<size_t>(READ_BYTES - total)));
    if (n == 0) {
      break;
    }
    crc = esp_rom_crc32_le(crc, buf, n);
    total += n;
    if (interleaveLcd && (total % 16384) == 0) {
      lcdFillRect(0, 300, SCREEN_W, 8, (total >> 14) & 1 ? 0x001F : 0xF800);
    }
  }
  const uint32_t us = micros() - t0;
  f.close();
  return {total == READ_BYTES, total * 976.5625f / us, crc};
}

static uint8_t patternByte(size_t offset) {
  return static_cast<uint8_t>(offset * 7 + (offset >> 12) * 13);
}

static bool writeTest(float &writeKbps, float &verifyKbps) {
  SD.mkdir(BENCH_ROOT);
  File f = SD.open(BENCH_ROOT "/w.bin", FILE_WRITE);
  if (!f) {
    return false;
  }
  uint32_t t0 = micros();
  for (size_t off = 0; off < WRITE_BYTES; off += sizeof(buf)) {
    for (size_t i = 0; i < sizeof(buf); i++) {
      buf[i] = patternByte(off + i);
    }
    if (f.write(buf, sizeof(buf)) != sizeof(buf)) {
      f.close();
      return false;
    }
  }
  f.close();
  writeKbps = WRITE_BYTES * 976.5625f / (micros() - t0);

  f = SD.open(BENCH_ROOT "/w.bin");
  if (!f) {
    return false;
  }
  bool ok = true;
  t0 = micros();
  for (size_t off = 0; off < WRITE_BYTES && ok; off += sizeof(buf)) {
    if (f.read(buf, sizeof(buf)) != sizeof(buf)) {
      ok = false;
      break;
    }
    for (size_t i = 0; i < sizeof(buf); i++) {
      if (buf[i] != patternByte(off + i)) {
        ok = false;
        break;
      }
    }
  }
  verifyKbps = WRITE_BYTES * 976.5625f / (micros() - t0);
  f.close();
  SD.remove(BENCH_ROOT "/w.bin");
  return ok;
}

// ---------------------------------------------------------------- Test library
static void putBe32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static size_t textFrame(uint8_t *out, const char *id, const String &text) {
  const uint32_t len = text.length() + 1;  // encoding byte + Latin-1 text
  memcpy(out, id, 4);
  putBe32(out + 4, len);
  out[8] = out[9] = 0;
  out[10] = 0;
  memcpy(out + 11, text.c_str(), text.length());
  return 10 + len;
}

static String benchDir(int d) {
  char path[32];
  snprintf(path, sizeof(path), BENCH_LIB "/d%02d", d);
  return path;
}

static String benchFile(int d, int i) {
  char name[64];
  snprintf(name, sizeof(name), "/Bench Song %05d.mp3", d * BENCH_FILES_PER_DIR + i);
  return benchDir(d) + name;
}

// Writes one fake MP3: ID3v2.3 tag with cover art FIRST (as many taggers do,
// the worst case for a tag reader), then text frames, padding, fake audio.
String benchSongPath(int d, int i) {
  return benchFile(d, i);
}

// Writes a synthetic test MP3 (see writeBenchFile) at `path` with `title`;
// artist, album and track are derived from n. Used by the indexer tests.
bool benchWriteSong(const String &path, const String &title, int n) {
  uint8_t frames[256];
  size_t textLen = 0;
  textLen += textFrame(frames + textLen, "TIT2", title);
  textLen += textFrame(frames + textLen, "TPE1", "Artist " + String(n % 97));
  textLen += textFrame(frames + textLen, "TALB", "Album " + String(n % 211));
  textLen += textFrame(frames + textLen, "TRCK", String(n % 12 + 1));

  static const uint8_t apicHead[] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0};
  const uint32_t apicBody = sizeof(apicHead) + BENCH_ART_BYTES;
  const uint32_t padding = 512;
  const uint32_t tagSize = 10 + apicBody + textLen + padding;

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    return false;
  }
  uint8_t h[20] = {'I', 'D', '3', 3, 0, 0};
  h[6] = (tagSize >> 21) & 0x7F;
  h[7] = (tagSize >> 14) & 0x7F;
  h[8] = (tagSize >> 7) & 0x7F;
  h[9] = tagSize & 0x7F;
  memcpy(h + 10, "APIC", 4);
  putBe32(h + 14, apicBody);
  h[18] = h[19] = 0;
  f.write(h, 20);
  f.write(apicHead, sizeof(apicHead));
  memset(buf, 0, sizeof(buf));
  for (uint32_t left = BENCH_ART_BYTES; left > 0;) {
    const uint32_t chunk = min(left, static_cast<uint32_t>(sizeof(buf)));
    f.write(buf, chunk);
    left -= chunk;
  }
  f.write(frames, textLen);
  f.write(buf, padding);
  f.write(buf, BENCH_AUDIO_BYTES);
  f.close();
  return true;
}

static bool writeBenchFile(int d, int i) {
  const int n = d * BENCH_FILES_PER_DIR + i;
  return benchWriteSong(benchFile(d, i), "Bench Song " + String(n), n);
}

static void ensureTestLibrary() {
  if (SD.exists(benchFile(BENCH_DIRS - 1, BENCH_FILES_PER_DIR - 1))) {
    Serial.println("[bench] test library already present");
    return;
  }
  SD.mkdir(BENCH_ROOT);
  SD.mkdir(BENCH_LIB);
  const uint32_t t0 = millis();
  for (int d = 0; d < BENCH_DIRS; d++) {
    status("Writing test files", String(d * BENCH_FILES_PER_DIR) + " / " +
                                 String(BENCH_DIRS * BENCH_FILES_PER_DIR));
    SD.mkdir(benchDir(d));
    for (int i = 0; i < BENCH_FILES_PER_DIR; i++) {
      if (!writeBenchFile(d, i)) {
        Serial.printf("[bench] write failed: %s\n", benchFile(d, i).c_str());
        return;
      }
    }
  }
  Serial.printf("[bench] wrote %d files in %lu ms\n", BENCH_DIRS * BENCH_FILES_PER_DIR,
                millis() - t0);
}

// ---------------------------------------------------------------- Listing
// Each returns ms per entry over the whole test library.
static float listOpenNextFile() {
  uint32_t count = 0;
  volatile uint32_t sink = 0;
  const uint32_t t0 = millis();
  for (int d = 0; d < BENCH_DIRS; d++) {
    File dir = SD.open(benchDir(d));
    while (true) {
      File e = dir.openNextFile();
      if (!e) {
        break;
      }
      sink += e.size() + e.getLastWrite();
      count++;
      e.close();
    }
    dir.close();
  }
  const uint32_t ms = millis() - t0;
  Serial.printf("[bench]   openNextFile+size+mtime: %lu entries, %lu ms\n", count, ms);
  return count ? static_cast<float>(ms) / count : 0;
}

static float listGetNextFileName() {
  uint32_t count = 0;
  const uint32_t t0 = millis();
  for (int d = 0; d < BENCH_DIRS; d++) {
    File dir = SD.open(benchDir(d));
    bool isDir = false;
    while (true) {
      const String name = dir.getNextFileName(&isDir);
      if (name.isEmpty()) {
        break;
      }
      count++;
    }
    dir.close();
  }
  const uint32_t ms = millis() - t0;
  Serial.printf("[bench]   getNextFileName (name only): %lu entries, %lu ms\n", count, ms);
  return count ? static_cast<float>(ms) / count : 0;
}

static float listFatFs() {
  uint32_t count = 0;
  volatile uint32_t sink = 0;
  static FILINFO info;
  static FF_DIR dir;
  const uint32_t t0 = millis();
  for (int d = 0; d < BENCH_DIRS; d++) {
    const String path = String(FATFS_DRIVE) + benchDir(d);
    if (f_opendir(&dir, path.c_str()) != FR_OK) {
      Serial.printf("[bench]   f_opendir failed: %s\n", path.c_str());
      return -1;
    }
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
      sink += info.fsize + info.fdate + info.ftime;
      count++;
    }
    f_closedir(&dir);
  }
  const uint32_t ms = millis() - t0;
  Serial.printf("[bench]   f_readdir+size+mtime: %lu entries, %lu ms\n", count, ms);
  return count ? static_cast<float>(ms) / count : 0;
}

// ---------------------------------------------------------------- Tags
#define TAG_SAMPLE_DIRS 3

static float tagParse(bool parse) {
  uint32_t count = 0;
  uint32_t found = 0;
  const uint32_t t0 = millis();
  for (int d = 0; d < TAG_SAMPLE_DIRS; d++) {
    for (int i = 0; i < BENCH_FILES_PER_DIR; i++) {
      File f = SD.open(benchFile(d, i));
      if (!f) {
        continue;
      }
      if (parse) {
        TagInfo tags;
        FsFileSource src(f);
        readTags(src, tags);
        if (!tags.title.isEmpty() && !tags.artist.isEmpty()) {
          found++;
        }
      } else {
        f.read(buf, 10);
      }
      f.close();
      count++;
    }
  }
  const uint32_t ms = millis() - t0;
  if (parse) {
    Serial.printf("[bench]   open+readTags: %lu files (%lu with title+artist), %lu ms\n",
                  count, found, ms);
  } else {
    Serial.printf("[bench]   open+read 10 bytes: %lu files, %lu ms\n", count, ms);
  }
  return count ? static_cast<float>(ms) / count : 0;
}

static float tagParseReal() {
  const int reps = 20;
  const uint32_t t0 = millis();
  for (int r = 0; r < reps; r++) {
    File f = SD.open(REAL_FILE);
    TagInfo tags;
    FsFileSource src(f);
    readTags(src, tags);
    f.close();
  }
  return static_cast<float>(millis() - t0) / reps;
}

// ---------------------------------------------------------------- Driver
struct IndexTimes {
  float openNext, nameOnly, fatfs, openOnly, tags, real;
};

static IndexTimes indexBenchmarks(uint32_t hz) {
  IndexTimes t;
  mountAt(hz);
  status("Timing listing", String(hz / 1000000) + " MHz");
  t.openNext = listOpenNextFile();
  t.nameOnly = listGetNextFileName();
  t.fatfs = listFatFs();
  status("Timing tags", String(hz / 1000000) + " MHz");
  t.openOnly = tagParse(false);
  t.tags = tagParse(true);
  t.real = tagParseReal();
  Serial.printf("[bench]   real file readTags: %.1f ms\n", t.real);
  return t;
}

static void runStep0Bench() {
  Serial.println();
  Serial.println("[bench] ===== step 0 benchmarks =====");
  Serial.printf("[bench] free heap %u, largest block %u\n", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  // --- SD clock sweep
  static const uint32_t speeds[] = {4000000, 8000000, 10000000, 16000000, 20000000};
  uint32_t baselineCrc = 0;
  uint32_t best = 4000000;
  Serial.println("[bench] SD clock sweep (read 1 MB of real MP3, write+verify 512 KB, read with LCD interleaved)");
  for (uint32_t hz : speeds) {
    status("SD speed test", String(hz / 1000000) + " MHz");
    if (!mountAt(hz)) {
      Serial.printf("[bench] %2lu MHz: mount FAILED\n", hz / 1000000);
      continue;
    }
    const ReadResult r = readTest(false);
    const ReadResult ri = readTest(true);
    if (hz == speeds[0]) {
      baselineCrc = r.crc;
    }
    float wk = 0, vk = 0;
    const bool wOk = writeTest(wk, vk);
    const bool pass = r.ok && ri.ok && r.crc == baselineCrc && ri.crc == baselineCrc && wOk;
    Serial.printf("[bench] %2lu MHz: read %6.0f KB/s, read+LCD %6.0f KB/s, write %6.0f KB/s, "
                  "verify %6.0f KB/s, crc %s, write %s -> %s\n",
                  hz / 1000000, r.kbps, ri.kbps, wk, vk,
                  (r.crc == baselineCrc && ri.crc == baselineCrc) ? "ok" : "MISMATCH",
                  wOk ? "ok" : "BAD", pass ? "PASS" : "FAIL");
    if (pass) {
      best = hz;
    }
  }
  Serial.printf("[bench] fastest passing SD clock: %lu MHz\n", best / 1000000);

  // --- Test library
  mountAt(best);
  ensureTestLibrary();

  // --- FatFs drive check
  FF_DIR root;
  Serial.printf("[bench] f_opendir(\"" FATFS_DRIVE "/\"): %s\n",
                f_opendir(&root, FATFS_DRIVE "/") == FR_OK ? "ok" : "FAILED");
  f_closedir(&root);

  // --- Listing and tag timing at the old and the best clock
  Serial.println("[bench] listing + tag timing at 4 MHz");
  const IndexTimes slow = indexBenchmarks(4000000);
  Serial.printf("[bench] listing + tag timing at %lu MHz\n", best / 1000000);
  const IndexTimes fast = indexBenchmarks(best);

  Serial.println("[bench] ===== summary (ms per file) =====");
  Serial.printf("[bench]                      4 MHz   %2lu MHz\n", best / 1000000);
  Serial.printf("[bench] openNextFile        %6.2f  %6.2f\n", slow.openNext, fast.openNext);
  Serial.printf("[bench] getNextFileName     %6.2f  %6.2f\n", slow.nameOnly, fast.nameOnly);
  Serial.printf("[bench] f_readdir           %6.2f  %6.2f\n", slow.fatfs, fast.fatfs);
  Serial.printf("[bench] open only           %6.2f  %6.2f\n", slow.openOnly, fast.openOnly);
  Serial.printf("[bench] open+readTags       %6.2f  %6.2f\n", slow.tags, fast.tags);
  Serial.printf("[bench] real MP3 readTags   %6.2f  %6.2f\n", slow.real, fast.real);
  Serial.printf("[bench] free heap %u\n", ESP.getFreeHeap());
  Serial.println("[bench] DONE");
  status("Benchmark done", "Results on serial");
}

// ================================================================ Step 1
// Compares the tag parser over Arduino fs::File vs direct FatFs (CardFile),
// checks both give identical results, and prints field coverage + samples.

static void collectMp3s(const String &path, uint8_t depth, bool benchOnly,
                        std::vector<String> &out, size_t limit) {
  FF_DIR dir;
  if (f_opendir(&dir, cardPath(path.isEmpty() ? "/" : path).c_str()) != FR_OK) {
    return;
  }
  static FILINFO info;
  while (out.size() < limit && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
    if (info.fattrib & (AM_HID | AM_SYS)) {
      continue;
    }
    const String full = path + "/" + info.fname;
    if (info.fattrib & AM_DIR) {
      const bool isBench = strcmp(info.fname, "_bench") == 0;
      if (depth > 0 && (isBench || !librarySkipFolder(info.fname)) && (benchOnly || !isBench)) {
        collectMp3s(full, depth - 1, benchOnly, out, limit);
      }
    } else if (libraryIsMp3Name(info.fname) && (!benchOnly || path.startsWith(BENCH_ROOT))) {
      out.push_back(full);
    }
  }
  f_closedir(&dir);
}

static bool sameTags(const TagInfo &a, const TagInfo &b) {
  return a.title == b.title && a.artist == b.artist && a.album == b.album &&
         a.albumArtist == b.albumArtist && a.track == b.track && a.disc == b.disc &&
         a.year == b.year && a.durationMs == b.durationMs && a.audioStart == b.audioStart &&
         a.durationSource == b.durationSource;
}

struct TagTiming {
  float fileAvg, fileMax, cardAvg, cardMax;
  uint32_t mismatches;
};

extern uint32_t tagPhaseUs[3];

static TagTiming timeTagReaders(const std::vector<String> &paths, std::vector<TagInfo> *keep) {
  TagTiming t = {0, 0, 0, 0, 0};
  uint64_t fileTotal = 0, cardTotal = 0;
  for (const String &path : paths) {
    TagInfo viaFile, viaCard;
    uint32_t t0 = micros();
    File f = SD.open(path);
    if (f) {
      FsFileSource src(f);
      readTags(src, viaFile);
      f.close();
    }
    uint32_t us = micros() - t0;
    fileTotal += us;
    t.fileMax = max(t.fileMax, us / 1000.0f);

    t0 = micros();
    CardFile card;
    if (card.open(path)) {
      readTags(card, viaCard);
      card.close();
    }
    us = micros() - t0;
    cardTotal += us;
    t.cardMax = max(t.cardMax, us / 1000.0f);

    if (!sameTags(viaFile, viaCard)) {
      t.mismatches++;
      Serial.printf("[bench]   MISMATCH %s\n", path.c_str());
    }
    if (keep) {
      keep->push_back(viaCard);
    }
  }
  if (!paths.empty()) {
    t.fileAvg = fileTotal / 1000.0f / paths.size();
    t.cardAvg = cardTotal / 1000.0f / paths.size();
  }
  return t;
}

static String mmss(uint32_t ms) {
  char text[12];
  snprintf(text, sizeof(text), "%lu:%02lu", ms / 60000, (ms / 1000) % 60);
  return text;
}

static void runStep1Bench() {
  Serial.println();
  Serial.println("[bench] ===== step 1: tag reader =====");
  mountAt(SD_SPI_HZ);

  status("Finding songs", "");
  std::vector<String> real, bench;
  collectMp3s("", 4, false, real, 5000);
  collectMp3s("", 4, true, bench, 300);
  Serial.printf("[bench] %u real songs, %u bench files\n", real.size(), bench.size());

  status("Timing tag reads", "real songs");
  std::vector<TagInfo> realTags;
  memset(tagPhaseUs, 0, sizeof(tagPhaseUs));
  const TagTiming tr = timeTagReaders(real, &realTags);
  // Both readers ran, so halve to get a per-read figure (mostly FatFs-like).
  const float phaseMs[3] = {tagPhaseUs[0] / 2000.0f / real.size(), tagPhaseUs[1] / 2000.0f / real.size(),
                            tagPhaseUs[2] / 2000.0f / real.size()};
  status("Timing tag reads", "bench files");
  const TagTiming tb = timeTagReaders(bench, nullptr);

  // Open-only cost (FatFs name lookup), to split open time from parse time.
  auto openOnly = [](const std::vector<String> &paths) {
    const uint32_t t0 = micros();
    for (const String &path : paths) {
      CardFile card;
      card.open(path);
    }
    return paths.empty() ? 0.0f : (micros() - t0) / 1000.0f / paths.size();
  };
  const float openReal = openOnly(real);
  const float openBench = openOnly(bench);

  // Field coverage over the real songs.
  uint32_t hasTitle = 0, hasArtist = 0, hasAlbum = 0, hasAlbumArtist = 0, hasTrack = 0,
           hasDisc = 0, hasYear = 0, minMs = UINT32_MAX, maxMs = 0;
  uint32_t bySource[DUR_CBR + 1] = {0};
  for (const TagInfo &t : realTags) {
    hasTitle += !t.title.isEmpty();
    hasArtist += !t.artist.isEmpty();
    hasAlbum += !t.album.isEmpty();
    hasAlbumArtist += !t.albumArtist.isEmpty();
    hasTrack += t.track > 0;
    hasDisc += t.disc > 0;
    hasYear += t.year > 0;
    bySource[t.durationSource]++;
    if (t.durationMs > 0) {
      minMs = min(minMs, t.durationMs);
      maxMs = max(maxMs, t.durationMs);
    }
  }

  Serial.println("[bench] samples (every ~10th real song):");
  for (size_t i = 0; i < real.size(); i += max<size_t>(1, real.size() / 18)) {
    const TagInfo &t = realTags[i];
    Serial.printf("[bench]   %s\n", real[i].c_str());
    Serial.printf("[bench]     \"%s\" / %s / %s / aa:%s / #%u d%u / %u / %s (%s) audio@%lu\n",
                  t.title.c_str(), t.artist.c_str(), t.album.c_str(), t.albumArtist.c_str(),
                  t.track, t.disc, t.year, mmss(t.durationMs).c_str(),
                  durationSourceName(t.durationSource), t.audioStart);
  }

  const size_t n = realTags.size();
  Serial.println("[bench] ===== step 1 summary =====");
  Serial.printf("[bench] real songs (%u):  fs::File avg %.2f max %.2f ms | FatFs avg %.2f max %.2f ms | mismatches %lu\n",
                n, tr.fileAvg, tr.fileMax, tr.cardAvg, tr.cardMax, tr.mismatches);
  Serial.printf("[bench] bench files (%u): fs::File avg %.2f max %.2f ms | FatFs avg %.2f max %.2f ms | mismatches %lu\n",
                bench.size(), tb.fileAvg, tb.fileMax, tb.cardAvg, tb.cardMax, tb.mismatches);
  Serial.printf("[bench] FatFs open only: real %.2f ms, bench %.2f ms  ->  parse: real %.2f ms, bench %.2f ms\n",
                openReal, openBench, tr.cardAvg - openReal, tb.cardAvg - openBench);
  Serial.printf("[bench] parse phases (real, avg of both readers): ID3v2 %.2f ms, ID3v1 %.2f ms, frame scan %.2f ms\n",
                phaseMs[0], phaseMs[1], phaseMs[2]);
  Serial.printf("[bench] coverage: title %lu, artist %lu, album %lu, album artist %lu, track %lu, disc %lu, year %lu of %u\n",
                hasTitle, hasArtist, hasAlbum, hasAlbumArtist, hasTrack, hasDisc, hasYear, n);
  Serial.printf("[bench] length from: xing %lu, vbri %lu, tlen %lu, cbr %lu, none %lu; range %s .. %s\n",
                bySource[DUR_XING], bySource[DUR_VBRI], bySource[DUR_TLEN], bySource[DUR_CBR],
                bySource[DUR_NONE], mmss(minMs == UINT32_MAX ? 0 : minMs).c_str(), mmss(maxMs).c_str());
  Serial.printf("[bench] free heap %u\n", ESP.getFreeHeap());
  Serial.println("[bench] DONE");
  status("Benchmark done", "Results on serial");
}

// ---------------------------------------------------------------- Large library
// Writes BIG_ARTISTS x BIG_ALBUMS x BIG_TRACKS small but realistic test songs
// under /_bench/big/<Artist>/<Album>/NN - Title.mp3 (~2 KB each, one 4 KB
// cluster). Deterministic names; skips albums already complete, so it can
// resume after an interruption.
#define BIG_ROOT "/_bench/big"
#define BIG_ARTISTS 1600
#define BIG_ALBUMS 3
#define BIG_TRACKS 10

static const char *const kAdjectives[40] = {
  "Amber", "Black", "Blue", "Broken", "Burning", "Cold", "Crimson", "Crystal", "Dark", "Distant",
  "Electric", "Empty", "Fading", "Falling", "Frozen", "Golden", "Hidden", "Hollow", "Iron", "Last",
  "Lonely", "Lost", "Midnight", "Neon", "Northern", "Paper", "Quiet", "Rapid", "Restless", "Silent",
  "Silver", "Small", "Velvet", "Violet", "Wild", "Winter", "Young", "Zero", "Yellow", "Kinetic"};
static const char *const kNouns[40] = {
  "Arrow", "Atlas", "Bridge", "Canyon", "Comet", "Crow", "Dream", "Echo", "Engine", "Falcon",
  "Fire", "Forest", "Garden", "Ghost", "Harbor", "Horizon", "Island", "Jungle", "Knight", "Lantern",
  "Machine", "Mirror", "Monarch", "Ocean", "Orbit", "Pilot", "Planet", "Raven", "River", "Rocket",
  "Shadow", "Signal", "Spark", "Stone", "Storm", "Tiger", "Union", "Valley", "Wave", "Wolf"};
static const char *const kWords[60] = {
  "Again", "Alive", "Angel", "Avenue", "Before", "Believe", "Blood", "Breathe", "Calling", "Chains",
  "City", "Closer", "Dancing", "Daylight", "Desire", "Down", "Every", "Forever", "Freedom", "Ghosts",
  "Gravity", "Heart", "Heaven", "Home", "Hurricane", "Into", "Jealous", "Just", "Kiss", "Lights",
  "Love", "Lullaby", "Memories", "Moonlight", "Never", "Night", "Nothing", "Over", "Paradise", "Promise",
  "Queen", "Rain", "Running", "Satellite", "Secrets", "Shine", "Something", "Summer", "Tonight", "Under",
  "Universe", "Waiting", "Waves", "When", "Without", "Wonder", "Xanadu", "Yesterday", "Zoo", "99"};

static uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

static String bigArtist(int a) {
  String name = String(kAdjectives[a % 40]) + " " + kNouns[(a / 40) % 40] + "s";
  return (mix(a) % 7 == 0) ? "The " + name : name;
}

static String bigAlbum(int a, int b) {
  const uint32_t h = mix(a * 31 + b);
  return String(kWords[h % 60]) + " " + kWords[(h >> 8) % 60];
}

static String bigTitle(int a, int b, int t) {
  const uint32_t h = mix((a * 31 + b) * 101 + t);
  return String(kWords[h % 60]) + " " + kWords[(h >> 8) % 60] + ((h >> 16) % 3 == 0 ? " " + String(kWords[(h >> 20) % 60]) : "");
}

// Builds one test MP3 in buf: ID3v2.3 (text frames, optional ~1 KB cover art)
// and two MPEG-1 Layer III frame headers, the first carrying a Xing frame
// count so the song has a length. Returns its size.
static size_t buildBigSong(uint8_t *out, int a, int b, int t) {
  const bool compilation = (a * BIG_ALBUMS + b) % 25 == 0;
  const int songNo = (a * BIG_ALBUMS + b) * BIG_TRACKS + t;
  const String artist = compilation ? bigArtist(mix(songNo) % BIG_ARTISTS) : bigArtist(a);
  uint8_t *p = out + 10;
  if (songNo % 97 != 0) p += textFrame(p, "TIT2", bigTitle(a, b, t));
  if (songNo % 89 != 0) p += textFrame(p, "TPE1", artist);
  p += textFrame(p, "TALB", bigAlbum(a, b));
  if (!compilation && a % 2 == 0) p += textFrame(p, "TPE2", bigArtist(a));
  p += textFrame(p, "TRCK", String(t + 1) + "/" + String(BIG_TRACKS));
  if (a % 3 == 0) p += textFrame(p, "TPOS", "1/1");
  p += textFrame(p, "TYER", String(1965 + mix(a * 7 + b) % 58));
  if (a % 2 == 1) {  // cover art before the end of the tag
    static const uint8_t apicHead[] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0};
    const uint32_t art = 1024;
    memcpy(p, "APIC", 4);
    putBe32(p + 4, sizeof(apicHead) + art);
    p[8] = p[9] = 0;
    memcpy(p + 10, apicHead, sizeof(apicHead));
    memset(p + 10 + sizeof(apicHead), 0, art);
    p += 10 + sizeof(apicHead) + art;
  }
  const uint32_t tagSize = (p - out) - 10;
  memcpy(out, "ID3\x03\x00\x00", 6);
  out[6] = (tagSize >> 21) & 0x7F;
  out[7] = (tagSize >> 14) & 0x7F;
  out[8] = (tagSize >> 7) & 0x7F;
  out[9] = tagSize & 0x7F;
  // MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo: 417-byte frames.
  const size_t frameLen = 417;
  memset(p, 0, frameLen + 4);
  static const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0x00};
  memcpy(p, header, 4);
  memcpy(p + 36, "Xing", 4);
  putBe32(p + 40, 1);                               // flags: frame count present
  putBe32(p + 44, 4000 + mix(songNo) % 9000);       // ~1:44 .. 5:39
  memcpy(p + frameLen, header, 4);
  p += frameLen + 4;
  return p - out;
}

static bool fatWrite(const String &path, const uint8_t *data, size_t len) {
  static FIL fil;
  UINT n = 0;
  if (f_open(&fil, cardPath(path).c_str(), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    return false;
  }
  const bool ok = f_write(&fil, data, len, &n) == FR_OK && n == len;
  return (f_close(&fil) == FR_OK) && ok;
}

static void runBigGenerator() {
  mountAt(SD_SPI_HZ);
  static uint8_t song[2600];
  f_mkdir(cardPath(BENCH_ROOT).c_str());
  f_mkdir(cardPath(BIG_ROOT).c_str());
  const uint32_t t0 = millis();
  uint32_t written = 0, skipped = 0, failed = 0;
  for (int a = 0; a < BIG_ARTISTS; a++) {
    const String artistDir = String(BIG_ROOT) + "/" + bigArtist(a);
    f_mkdir(cardPath(artistDir).c_str());
    for (int b = 0; b < BIG_ALBUMS; b++) {
      const String albumDir = artistDir + "/" + bigAlbum(a, b) + " (" + String(b + 1) + ")";
      auto songPath = [&](int t) {
        char num[8];
        snprintf(num, sizeof(num), "%02d - ", t + 1);
        return albumDir + "/" + num + bigTitle(a, b, t) + ".mp3";
      };
      FILINFO info;
      if (f_stat(cardPath(songPath(BIG_TRACKS - 1)).c_str(), &info) == FR_OK) {
        skipped += BIG_TRACKS;  // album already complete
        continue;
      }
      f_mkdir(cardPath(albumDir).c_str());
      for (int t = 0; t < BIG_TRACKS; t++) {
        const size_t len = buildBigSong(song, a, b, t);
        if (fatWrite(songPath(t), song, len)) {
          written++;
        } else {
          failed++;
        }
      }
    }
    if (a % 20 == 19) {
      const uint32_t done = (a + 1) * BIG_ALBUMS * BIG_TRACKS;
      Serial.printf("[big] %lu / %u songs (%lu written, %lu skipped, %lu failed), %lu s\n", done,
                    BIG_ARTISTS * BIG_ALBUMS * BIG_TRACKS, written, skipped, failed,
                    (millis() - t0) / 1000);
      status("Writing test songs", String(done) + " / " + String(BIG_ARTISTS * BIG_ALBUMS * BIG_TRACKS));
    }
  }
  Serial.printf("[big] DONE: %lu written, %lu skipped, %lu failed in %lu s\n", written, skipped,
                failed, (millis() - t0) / 1000);
  status("Test songs done", String(written) + " written");
}

// Card tools for testing the player's background indexing:
//   5 adds "/Zz Background Test.mp3", 6 removes it, 7 deletes the library,
//   8 prints card geometry (FAT type, cluster size, free space),
//   11 moves the 48k generated songs to "/Big Test" (visible to the player),
//   12 moves them back under /_bench (hidden from the player).
static void runCardTool() {
  for (uint32_t t0 = millis(); !Serial.available() && millis() - t0 < 30000;) {
    delay(10);  // wait for a line on serial (monitor attached), up to 30 s
  }
  mountAt(SD_SPI_HZ);
  const char *path = "/Zz Background Test.mp3";
#if RUN_BENCH == 5
  const bool ok = benchWriteSong(path, "Zz Background Test", 1);
  Serial.printf("[tool] added %s: %s\n", path, ok ? "ok" : "FAILED");
#elif RUN_BENCH == 6
  const bool ok = SD.remove(path);
  Serial.printf("[tool] removed %s: %s\n", path, ok ? "ok" : "FAILED");
#elif RUN_BENCH == 7
  LibStore::removeAllExcept(LIB_NONE);
  Serial.printf("[tool] deleted library generations; current now %s\n",
                LibStore::findCurrent() == LIB_NONE ? "none" : "STILL PRESENT");
#elif RUN_BENCH == 11 || RUN_BENCH == 12
  const char *from = RUN_BENCH == 11 ? "0:/_bench/big" : "0:/Big Test";
  const char *to = RUN_BENCH == 11 ? "0:/Big Test" : "0:/_bench/big";
  const FRESULT fr = f_rename(from, to);
  Serial.printf("[tool] moved %s to %s: %s (FatFs %d)\n", from, to, fr == FR_OK ? "ok" : "FAILED", fr);
#else
  FATFS *fs = nullptr;
  DWORD freeClusters = 0;
  f_getfree(CARD_DRIVE, &freeClusters, &fs);
  if (fs) {
    const uint32_t clusterBytes = fs->csize * fs->ssize;
    Serial.printf("[tool] FAT type %u (1=12,2=16,3=32), cluster %lu bytes, clusters %lu, free %lu "
                  "(%lu MB free of %lu MB)\n",
                  fs->fs_type, clusterBytes, fs->n_fatent - 2, freeClusters,
                  static_cast<uint32_t>(static_cast<uint64_t>(freeClusters) * clusterBytes >> 20),
                  static_cast<uint32_t>(static_cast<uint64_t>(fs->n_fatent - 2) * clusterBytes >> 20));
  }
#endif
  status("Card tool done", "");
}


// ---------------------------------------------------------------- SD clock check (13)
// Is a faster SD clock safe? At each speed: re-read a 4 MB pattern file
// (plain and with LCD draws interleaved) checking every byte, write+verify
// 1 MB, then list folders and read tags of ~1000 songs and compare a hash of
// everything seen with the 20 MHz run. Each speed runs twice.
#define PATTERN_FILE BENCH_ROOT "/pattern.bin"
#define PATTERN_BYTES (4 * 1024 * 1024)

static bool makePatternFile() {
  SD.mkdir(BENCH_ROOT);
  File f = SD.open(PATTERN_FILE);
  if (f && f.size() == PATTERN_BYTES) {
    f.close();
    return true;
  }
  if (f) {
    f.close();
  }
  f = SD.open(PATTERN_FILE, FILE_WRITE);
  if (!f) {
    return false;
  }
  for (size_t off = 0; off < PATTERN_BYTES; off += sizeof(buf)) {
    for (size_t i = 0; i < sizeof(buf); i++) {
      buf[i] = patternByte(off + i);
    }
    if (f.write(buf, sizeof(buf)) != sizeof(buf)) {
      f.close();
      return false;
    }
  }
  f.close();
  return true;
}

// Returns KB/s, or -1 on any byte mismatch / short read.
static float patternRead(bool interleaveLcd) {
  File f = SD.open(PATTERN_FILE);
  if (!f) {
    return -1;
  }
  const uint32_t t0 = micros();
  for (size_t off = 0; off < PATTERN_BYTES; off += sizeof(buf)) {
    if (f.read(buf, sizeof(buf)) != sizeof(buf)) {
      f.close();
      return -1;
    }
    for (size_t i = 0; i < sizeof(buf); i++) {
      if (buf[i] != patternByte(off + i)) {
        Serial.printf("[speed]   mismatch at byte %u\n", off + i);
        f.close();
        return -1;
      }
    }
    if (interleaveLcd && (off % 16384) == 0) {
      lcdFillRect(0, 300, SCREEN_W, 8, (off >> 14) & 1 ? 0x001F : 0xF800);
    }
  }
  const uint32_t us = micros() - t0;
  f.close();
  return PATTERN_BYTES * 976.5625f / us;
}

static uint32_t hashBytes(uint32_t h, const void *p, size_t n) {
  return esp_rom_crc32_le(h, static_cast<const uint8_t *>(p), n);
}

// Lists the first `artists` artist folders of the generated songs and reads
// every song's tags. Returns a hash of names, sizes and tag fields.
static uint32_t tagWalk(const char *root, uint32_t artists, uint32_t &songs, uint32_t &ms) {
  uint32_t h = 0;
  songs = 0;
  const uint32_t t0 = millis();
  static FILINFO info;
  FF_DIR top;
  if (f_opendir(&top, (String(FATFS_DRIVE) + root).c_str()) != FR_OK) {
    return 0;
  }
  std::vector<String> artistNames;
  while (artistNames.size() < artists && f_readdir(&top, &info) == FR_OK && info.fname[0]) {
    if (info.fattrib & AM_DIR) {
      artistNames.push_back(info.fname);
    }
  }
  f_closedir(&top);
  CardFile card;
  for (const String &a : artistNames) {
    const String artistPath = String(root) + "/" + a;
    FF_DIR ad;
    if (f_opendir(&ad, cardPath(artistPath).c_str()) != FR_OK) {
      return 0;
    }
    std::vector<String> albums;
    while (f_readdir(&ad, &info) == FR_OK && info.fname[0]) {
      if (info.fattrib & AM_DIR) {
        albums.push_back(info.fname);
      }
    }
    f_closedir(&ad);
    for (const String &al : albums) {
      const String albumPath = artistPath + "/" + al;
      FF_DIR d;
      if (f_opendir(&d, cardPath(albumPath).c_str()) != FR_OK) {
        return 0;
      }
      while (f_readdir(&d, &info) == FR_OK && info.fname[0]) {
        if (info.fattrib & AM_DIR) {
          continue;
        }
        h = hashBytes(h, info.fname, strlen(info.fname));
        h = hashBytes(h, &info.fsize, sizeof(info.fsize));
        uint32_t cluster = 0;
        if (!cardCaptureEntry(d, info, cluster) || !card.openCluster(cluster, info.fsize)) {
          if (!card.open(albumPath + "/" + info.fname)) {
            continue;
          }
        }
        TagInfo t;
        readTags(card, t);
        card.close();
        h = hashBytes(h, t.title.c_str(), t.title.length());
        h = hashBytes(h, t.artist.c_str(), t.artist.length());
        h = hashBytes(h, t.album.c_str(), t.album.length());
        h = hashBytes(h, &t.durationMs, sizeof(t.durationMs));
        songs++;
      }
      f_closedir(&d);
    }
  }
  ms = millis() - t0;
  return h;
}

static void runSpeedBench() {
  Serial.println();
  Serial.println("[speed] ===== SD clock check =====");
  const char *root = SD.exists("/Big Test") ? "/Big Test" : "/_bench/big";
  static const uint32_t speeds[] = {20000000, 26666667, 40000000};
  uint32_t baseHash = 0, baseSongs = 0;
  bool allPass[3] = {true, true, true};
  mountAt(speeds[0]);
  if (!makePatternFile()) {
    Serial.println("[speed] cannot create the pattern file");
    return;
  }
  for (int round = 0; round < 2; round++) {
    for (int i = 0; i < 3; i++) {
      const uint32_t hz = speeds[i];
      const float mhz = hz / 1e6f;
      status("SD clock check", String(mhz, 1) + " MHz");
      if (!mountAt(hz)) {
        Serial.printf("[speed] %4.1f MHz: mount FAILED\n", mhz);
        allPass[i] = false;
        continue;
      }
      const float r1 = patternRead(false), r2 = patternRead(false), r3 = patternRead(true);
      float wk = 0, vk = 0;
      const bool wOk = writeTest(wk, vk);
      uint32_t songs = 0, ms = 0;
      const uint32_t h = tagWalk(root, 33, songs, ms);
      if (i == 0 && round == 0) {
        baseHash = h;
        baseSongs = songs;
      }
      const bool tagsOk = songs > 0 && songs == baseSongs && h == baseHash;
      const bool pass = r1 > 0 && r2 > 0 && r3 > 0 && wOk && tagsOk;
      allPass[i] = allPass[i] && pass;
      Serial.printf("[speed] %4.1f MHz round %d: read %4.0f/%4.0f KB/s, read+LCD %4.0f KB/s, "
                    "write %4.0f KB/s (verify %s), %lu songs listed+tagged in %lu ms "
                    "(%.2f ms/song, %s) -> %s\n",
                    mhz, round + 1, r1, r2, r3, wk, wOk ? "ok" : "BAD", songs, ms,
                    songs ? static_cast<float>(ms) / songs : 0.0f,
                    tagsOk ? "same as 20 MHz" : "DIFFERENT", pass ? "PASS" : "FAIL");
    }
  }
  for (int i = 0; i < 3; i++) {
    Serial.printf("[speed] %4.1f MHz: %s\n", speeds[i] / 1e6f, allPass[i] ? "PASS" : "FAIL");
  }
  mountAt(SD_SPI_HZ);
  Serial.println("[speed] DONE");
  status("SD clock check done", "Results on serial");
}

void runStep2Bench();  // BenchStore.cpp
void runStep3Bench();  // BenchIndex.cpp
void runScaleBench();  // BenchIndex.cpp
void runSortProfile(); // BenchIndex.cpp

void runBench() {
#if RUN_BENCH == 1
  runStep0Bench();
#elif RUN_BENCH == 2
  runStep1Bench();
#elif RUN_BENCH == 3
  runStep2Bench();
#elif RUN_BENCH == 4
  runStep3Bench();
#elif RUN_BENCH == 9
  runBigGenerator();
#elif RUN_BENCH == 10
  runScaleBench();
#elif RUN_BENCH == 13
  runSpeedBench();
#elif RUN_BENCH == 14
  runSortProfile();
#else
  runCardTool();
#endif
}

#endif  // RUN_BENCH
