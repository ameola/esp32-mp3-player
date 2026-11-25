// Step-2 harness: builds a library generation from the card (real songs plus
// /_bench), then checks read-back, page-read latency, crash safety and
// corruption detection. Build with -DRUN_BENCH=3.
#if defined(RUN_BENCH) && RUN_BENCH == 3

#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <deque>
#include <vector>
#include "ff.h"

#include "CardFile.h"
#include "Config.h"
#include "LibStore.h"
#include "Library.h"
#include "Tags.h"
#include "Ui.h"

static void say(const char *line1, const String &line2) {
  Serial.printf("[store] %s %s\n", line1, line2.c_str());
  uiShowMessage(line1, line2.c_str());
}

// ---------------------------------------------------------------- Build
struct TitleKey {
  uint8_t bucket;
  char key[19];  // lowercase title prefix
  uint32_t id;
};
static_assert(sizeof(TitleKey) == 24, "TitleKey size");

struct PendingFolder {
  uint32_t id;
  uint32_t parent;
  uint16_t depth;
  String path;  // Arduino-style, "" for root
  String name;
};

struct BuildStats {
  uint32_t songs = 0, folders = 0, nameBytes = 0;
  uint32_t scanMs = 0, indexMs = 0, commitMs = 0;
};

static bool caseLess(const String &a, const String &b) {
  return strcasecmp(a.c_str(), b.c_str()) < 0;
}

// Breadth-first walk: a folder's children get consecutive ids when the folder
// is processed, and its songs get consecutive ids, so both are contiguous.
static bool buildGeneration(uint32_t gen, BuildStats &stats, uint32_t stopAfterSongs = 0) {
  LibWriter names, folders, songs;
  if (!names.create(gen, LF_NAMES, 1) || !folders.create(gen, LF_FOLDERS, sizeof(FolderRecord)) ||
      !songs.create(gen, LF_SONGS, sizeof(SongRecord))) {
    return false;
  }
  // Reserved up front: letting the vector double past ~2000 keys needs a
  // ~100 KB contiguous block, which the heap doesn't have. (Step 3 sorts on
  // the card instead, so large libraries never hold all keys in RAM.)
  std::vector<TitleKey> keys;
  keys.reserve(2600);
  std::deque<PendingFolder> queue;
  queue.push_back({0, LIB_NONE, 0, "", "/"});
  uint32_t nextFolderId = 1;
  uint32_t nameBytes = 0;
  const uint32_t t0 = millis();
  static FILINFO info;

  while (!queue.empty()) {
    PendingFolder folder = queue.front();
    queue.pop_front();

    std::vector<String> subdirs;
    struct FileEntry { String name; uint32_t size, mtime; };
    std::vector<FileEntry> files;
    FF_DIR dir;
    if (f_opendir(&dir, cardPath(folder.path.isEmpty() ? "/" : folder.path).c_str()) == FR_OK) {
      while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        if (info.fattrib & (AM_HID | AM_SYS)) {
          continue;
        }
        if (info.fattrib & AM_DIR) {
          const bool isBench = strcmp(info.fname, "_bench") == 0;  // indexed here on purpose
          if (folder.depth < 8 && (isBench || !librarySkipFolder(info.fname))) {
            subdirs.push_back(info.fname);
          }
        } else if (libraryIsMp3Name(info.fname)) {
          files.push_back({info.fname, static_cast<uint32_t>(info.fsize),
                           (static_cast<uint32_t>(info.fdate) << 16) | info.ftime});
        }
      }
      f_closedir(&dir);
    }
    std::sort(subdirs.begin(), subdirs.end(), caseLess);
    std::sort(files.begin(), files.end(),
              [](const FileEntry &a, const FileEntry &b) { return caseLess(a.name, b.name); });

    FolderRecord frec = {};
    frec.parentId = folder.parent;
    frec.pathOffset = nameBytes;
    frec.pathLen = folder.path.length();
    frec.depth = folder.depth;
    frec.firstChild = nextFolderId;
    frec.childCount = subdirs.size();
    frec.firstSong = songs.count();
    frec.songCount = files.size();
    libCopyField(frec.name, sizeof(frec.name), folder.name);
    names.append(folder.path.c_str(), folder.path.length());
    nameBytes += folder.path.length();
    if (!folders.append(&frec)) {
      return false;
    }
    for (const String &sub : subdirs) {
      queue.push_back({nextFolderId++, folder.id, static_cast<uint16_t>(folder.depth + 1),
                       folder.path + "/" + sub, sub});
    }

    for (const FileEntry &file : files) {
      const String path = folder.path + "/" + file.name;
      TagInfo tags;
      CardFile card;
      if (card.open(path)) {
        readTags(card, tags);
        card.close();
      }
      SongRecord rec = {};
      rec.folderId = folder.id;
      rec.nameOffset = nameBytes;
      rec.nameLen = file.name.length();
      rec.fileSize = file.size;
      rec.mtime = file.mtime;
      rec.artistId = rec.albumId = LIB_NONE;  // grouping comes in step 3
      libFillSong(rec, tags, file.name.c_str());
      names.append(file.name.c_str(), file.name.length());
      nameBytes += file.name.length();

      TitleKey key = {};
      key.bucket = libBucket(rec.title);
      for (size_t i = 0; i < sizeof(key.key) - 1 && rec.title[i]; i++) {
        key.key[i] = tolower(static_cast<unsigned char>(rec.title[i]));
      }
      key.id = songs.count();
      keys.push_back(key);
      if (!songs.append(&rec)) {
        return false;
      }
      if (songs.count() % 250 == 0) {
        say("Building library", String(songs.count()) + " songs");
      }
      if (stopAfterSongs && songs.count() >= stopAfterSongs) {
        return false;  // simulated crash: files left unfinished, no manifest
      }
    }
  }
  stats.scanMs = millis() - t0;
  stats.songs = songs.count();
  stats.folders = folders.count();
  stats.nameBytes = nameBytes;
  if (!names.finish() || !folders.finish() || !songs.finish()) {
    return false;
  }

  // Title index (in RAM here; step 3 sorts on the card for large libraries).
  const uint32_t t1 = millis();
  std::sort(keys.begin(), keys.end(), [](const TitleKey &a, const TitleKey &b) {
    if (a.bucket != b.bucket) return a.bucket < b.bucket;
    const int c = strcmp(a.key, b.key);
    return c != 0 ? c < 0 : a.id < b.id;
  });
  JumpTable jump = {};
  LibWriter index;
  if (!index.create(gen, LF_TITLE_INDEX, sizeof(uint32_t), sizeof(JumpTable))) {
    return false;
  }
  for (int b = 0, pos = 0; b < 28; b++) {
    while (pos < static_cast<int>(keys.size()) && keys[pos].bucket < b) pos++;
    jump.start[b] = b == 27 ? keys.size() : pos;
  }
  for (const TitleKey &key : keys) {
    index.append(&key.id);
  }
  if (!index.finish(&jump)) {
    return false;
  }
  // Artists, albums and their indexes are built in step 3; write them empty
  // so the generation has its full set of files.
  const JumpTable empty = {};
  LibWriter w;
  if (!w.create(gen, LF_ARTISTS, sizeof(ArtistRecord)) || !w.finish() ||
      !w.create(gen, LF_ALBUMS, sizeof(AlbumRecord)) || !w.finish() ||
      !w.create(gen, LF_ARTIST_INDEX, sizeof(uint32_t), sizeof(JumpTable)) || !w.finish(&empty) ||
      !w.create(gen, LF_ALBUM_INDEX, sizeof(uint32_t), sizeof(JumpTable)) || !w.finish(&empty) ||
      !w.create(gen, LF_NONE_INDEX, sizeof(uint32_t)) || !w.finish()) {
    return false;
  }
  stats.indexMs = millis() - t1;

  const uint32_t t2 = millis();
  ManifestRecord manifest = {};
  manifest.songCount = stats.songs;
  manifest.folderCount = stats.folders;
  manifest.nameBytes = stats.nameBytes;
  manifest.buildMs = millis() - t0;
  const bool ok = LibStore::commit(gen, manifest);
  stats.commitMs = millis() - t2;
  return ok;
}

// ---------------------------------------------------------------- Checks
static String songPath(LibReader &folders, LibReader &names, const SongRecord &rec) {
  FolderRecord folder;
  char buf[512];
  if (!folders.read(rec.folderId, &folder) || folder.pathLen >= sizeof(buf)) {
    return "";
  }
  names.readBytes(sizeof(LibHeader) + folder.pathOffset, buf, folder.pathLen);
  String path(buf, folder.pathLen);
  if (rec.nameLen >= sizeof(buf)) {
    return "";
  }
  names.readBytes(sizeof(LibHeader) + rec.nameOffset, buf, rec.nameLen);
  return path + "/" + String(buf, rec.nameLen);
}

// Re-reads tags for a sample of songs via their reconstructed paths and
// compares with the stored records.
static uint32_t checkSample(uint32_t gen, uint32_t samples, uint32_t &checked) {
  LibReader songs, folders, names;
  if (!songs.open(gen, LF_SONGS, sizeof(SongRecord)) ||
      !folders.open(gen, LF_FOLDERS, sizeof(FolderRecord)) || !names.open(gen, LF_NAMES, 1)) {
    return UINT32_MAX;
  }
  uint32_t bad = 0;
  checked = 0;
  for (uint32_t i = 0; i < samples && songs.count() > 0; i++) {
    const uint32_t id = esp_random() % songs.count();
    SongRecord rec;
    songs.read(id, &rec);
    const String path = songPath(folders, names, rec);
    TagInfo tags;
    CardFile card;
    SongRecord again = {};
    if (card.open(path)) {
      readTags(card, tags);
      const char *name = path.c_str() + path.lastIndexOf('/') + 1;
      libFillSong(again, tags, name);
    }
    checked++;
    if (strcmp(again.title, rec.title) != 0 || again.durationMs != rec.durationMs ||
        card.size() != rec.fileSize) {
      bad++;
      Serial.printf("[store]   MISMATCH id %lu: %s\n", id, path.c_str());
    }
  }
  return bad;
}

// Random "page" reads: 6 ids from title.idx, then their 6 song records.
static void pageLatency(uint32_t gen, float &avgMs, float &maxMs) {
  LibReader index, songs;
  index.open(gen, LF_TITLE_INDEX, sizeof(uint32_t));
  songs.open(gen, LF_SONGS, sizeof(SongRecord));
  const int pages = 200;
  uint64_t total = 0;
  maxMs = 0;
  for (int p = 0; p < pages; p++) {
    const uint32_t pos = esp_random() % max<uint32_t>(1, index.count() - 6);
    const uint32_t t0 = micros();
    uint32_t ids[6];
    index.read(pos, ids, 6);
    SongRecord rec;
    for (uint32_t id : ids) {
      songs.read(id, &rec);
    }
    const uint32_t us = micros() - t0;
    total += us;
    maxMs = max(maxMs, us / 1000.0f);
  }
  avgMs = total / 1000.0f / pages;
}

// Flips one byte at `offset` in a file (call twice to restore).
static bool flipByte(const String &path, uint32_t offset) {
  static FIL fil;  // 4 KB: keep off the stack
  if (f_open(&fil, cardPath(path).c_str(), FA_READ | FA_WRITE) != FR_OK) {
    return false;
  }
  uint8_t b = 0;
  UINT n = 0;
  bool ok = f_lseek(&fil, offset) == FR_OK && f_read(&fil, &b, 1, &n) == FR_OK && n == 1;
  b ^= 0x5A;
  ok = ok && f_lseek(&fil, offset) == FR_OK && f_write(&fil, &b, 1, &n) == FR_OK && n == 1;
  f_close(&fil);
  return ok;
}

static void listGenerationsOnCard() {
  FF_DIR dir;
  static FILINFO info;
  String names;
  if (f_opendir(&dir, cardPath(LIB_DIR).c_str()) == FR_OK) {
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
      names += String(info.fname) + " ";
    }
    f_closedir(&dir);
  }
  Serial.printf("[store]   on card: %s\n", names.isEmpty() ? "(nothing)" : names.c_str());
}

static const char *passFail(bool pass) {
  return pass ? "PASS" : "FAIL";
}

void runStep2Bench() {
  Serial.println();
  Serial.println("[store] ===== step 2: on-card library format =====");
  SD.end();
  SD.begin(SD_CS, SPI, SD_SPI_HZ);

  ManifestRecord manifest;
  const uint32_t atBoot = LibStore::findCurrent(&manifest);
  if (atBoot == LIB_NONE) {
    Serial.println("[store] at boot: no valid library");
  } else {
    Serial.printf("[store] at boot: generation %lu valid, %lu songs, %lu folders (persisted from last run)\n",
                  atBoot, manifest.songCount, manifest.folderCount);
  }
  listGenerationsOnCard();

  // 1. Build and commit a new generation.
  const uint32_t gen = LibStore::newGeneration();
  say("Building library", "generation " + String(gen));
  BuildStats stats;
  const bool built = buildGeneration(gen, stats);
  const uint32_t current = LibStore::findCurrent(&manifest);
  const bool test1 = built && current == gen && manifest.songCount == stats.songs;
  Serial.printf("[store] 1 build+commit gen %lu: %lu songs, %lu folders, %lu name bytes; "
                "scan+tags %lu ms, index %lu ms, verify+commit %lu ms -> %s\n",
                gen, stats.songs, stats.folders, stats.nameBytes, stats.scanMs, stats.indexMs,
                stats.commitMs, passFail(test1));
  if (atBoot != LIB_NONE) {
    LibStore::removeAllExcept(gen);
    Serial.println("[store]   removed previous generation(s)");
  }
  listGenerationsOnCard();

  // 2. Full verification and sample comparison against the files themselves.
  say("Verifying", "");
  uint32_t t0 = millis();
  const bool verified = LibStore::verifyGeneration(gen);
  const uint32_t verifyMs = millis() - t0;
  uint32_t checked = 0;
  const uint32_t bad = checkSample(gen, 60, checked);
  Serial.printf("[store] 2 full CRC verify %lu ms; re-read %lu random songs via stored paths, %lu mismatches -> %s\n",
                verifyMs, checked, bad, passFail(verified && bad == 0));

  // 3. Page reads and jump table.
  float avgMs = 0, maxMs = 0;
  pageLatency(gen, avgMs, maxMs);
  LibReader index;
  JumpTable jump;
  index.open(gen, LF_TITLE_INDEX, sizeof(uint32_t));
  index.readExtra(&jump);
  String buckets;
  for (int b = 0; b < 27; b++) {
    const uint32_t n = jump.start[b + 1] - jump.start[b];
    if (n) {
      buckets += String(b == 0 ? '#' : static_cast<char>('A' + b - 1)) + ":" + String(n) + " ";
    }
  }
  index.close();
  Serial.printf("[store] 3 page read (6 ids + 6 songs): avg %.2f ms, max %.2f ms\n", avgMs, maxMs);
  Serial.printf("[store]   title jump table: %s\n", buckets.c_str());

  // 4. Crash while building the next generation: must be ignored, then cleaned up.
  say("Crash test", "");
  const uint32_t crashGen = LibStore::newGeneration();
  BuildStats ignored;
  buildGeneration(crashGen, ignored, 300);  // stops mid-build, no manifest
  const uint32_t afterCrash = LibStore::findCurrent();
  listGenerationsOnCard();
  LibStore::removeAllExcept(afterCrash);
  Serial.printf("[store] 4 crash mid-build of gen %lu: current is %lu (expected %lu) -> %s\n",
                crashGen, afterCrash, gen, passFail(afterCrash == gen));
  listGenerationsOnCard();

  // 5. Corruption: one flipped byte in songs.dat is caught by the data CRC;
  //    one flipped byte in the manifest header invalidates the generation.
  say("Corruption test", "");
  const String songsPath = LibStore::filePath(gen, LF_SONGS);
  const uint32_t mid = sizeof(LibHeader) + (stats.songs / 2) * sizeof(SongRecord) + 10;
  flipByte(songsPath, mid);
  const bool headerStillOk = LibStore::validateGeneration(gen);
  const bool dataCaught = !LibStore::verifyGeneration(gen);
  flipByte(songsPath, mid);  // restore
  const String manifestPath = LibStore::filePath(gen, LF_MANIFEST);
  flipByte(manifestPath, 8);
  const bool manifestCaught = LibStore::findCurrent() == LIB_NONE;
  flipByte(manifestPath, 8);  // restore
  const bool restored = LibStore::verifyGeneration(gen) && LibStore::findCurrent() == gen;
  Serial.printf("[store] 5 corrupt song data: header check %s, CRC verify %s; corrupt manifest: %s; restored: %s -> %s\n",
                headerStillOk ? "passes (expected)" : "fails", dataCaught ? "catches it" : "MISSES it",
                manifestCaught ? "generation rejected" : "NOT rejected", restored ? "ok" : "NO",
                passFail(dataCaught && manifestCaught && restored));

  // Sizes.
  Serial.println("[store] file sizes:");
  for (uint8_t t = LF_MANIFEST; t < LF_TYPE_COUNT; t++) {
    FILINFO info;
    if (f_stat(cardPath(LibStore::filePath(gen, static_cast<LibFileType>(t))).c_str(), &info) == FR_OK) {
      Serial.printf("[store]   %-13s %8lu bytes\n", kLibFileNames[t], static_cast<uint32_t>(info.fsize));
    }
  }
  Serial.printf("[store] free heap %u\n", ESP.getFreeHeap());
  Serial.println("[store] DONE");
  say("Store test done", "Results on serial");
}

#endif  // RUN_BENCH == 3
