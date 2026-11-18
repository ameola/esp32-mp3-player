// Step-3 harness: full index, integrity checks, unchanged re-run,
// add/delete/retag detection, crash + resume. Build with -DRUN_BENCH=4.
#if defined(RUN_BENCH) && (RUN_BENCH == 4 || RUN_BENCH == 10 || RUN_BENCH == 14)

#include <SD.h>
#include <SPI.h>
#include <esp_rom_crc.h>
#include <vector>
#include "ff.h"

#include "CardFile.h"
#include "Config.h"
#include "Indexer.h"
#include "LibStore.h"
#include "Library.h"
#include "Tags.h"
#include "Ui.h"

String benchSongPath(int d, int i);                                // Bench.cpp
bool benchWriteSong(const String &path, const String &title, int n);  // Bench.cpp

static void say(const char *line1, const String &line2) {
  Serial.printf("[index] %s %s\n", line1, line2.c_str());
  uiShowMessage(line1, line2.c_str());
}

static void progress(const char *phase, uint32_t done, uint32_t total, const char *) {
  static uint32_t last = 0;
  if (millis() - last < 1000) {
    return;
  }
  last = millis();
  uiShowStatus(phase, total ? (String(done) + " / " + String(total)).c_str() : "");
}

static const char *passFail(bool pass) {
  return pass ? "PASS" : "FAIL";
}

static void printStats(const char *label, const IndexStats &s) {
  Serial.printf("[index] %s: ok=%d unchanged=%d resumed=%d(at %lu) gen %lu | %lu songs, %lu folders, "
                "%lu artists, %lu albums | reused %lu, tagged %lu (fast %lu, path %lu, failed %lu) | "
                "walk %lu ms, songs %lu ms, sort %lu ms (%lu runs), commit %lu ms | min heap %lu%s%s\n",
                label, s.ok, s.unchanged, s.resumed, s.resumedAt, s.gen, s.songs, s.folders,
                s.artists, s.albums, s.reused, s.tagged, s.fastOpens, s.pathOpens, s.failedOpens,
                s.walkMs, s.songsMs, s.sortMs, s.sortRuns, s.commitMs, s.minFreeHeap,
                s.error.isEmpty() ? "" : " | error: ", s.error.c_str());
  if (!s.unchanged) {
    Serial.printf("[index]   breakdown: tags %lu ms of songs; sort %s (kept %lu songs) = feed %lu + "
                  "title idx %lu + artists %lu + album idx %lu ms; sort buffers %lu KB\n",
                  s.tagMs, s.sortPatched ? "PATCHED" : "full", s.keptSongs, s.feedMs,
                  s.titleIdxMs, s.artistIdxMs, s.albumIdxMs, s.sortBufferBytes / 1024);
    Serial.printf("[index]   feed: song reads %lu ms, sorter adds (incl. spills) %lu ms; largest "
                  "free block at sort %lu KB\n",
                  s.feedReadMs, s.feedAddMs, s.largestBlockAtSort / 1024);
  }
}

static bool runIndex(const char *label, IndexStats &s, bool forceFull, uint32_t stopAfter = 0,
                     bool fullSort = false) {
  IndexOptions o;
  o.includeBench = true;
  o.forceFull = forceFull;
  o.fullSort = fullSort;
  o.stopAfterSongs = stopAfter;
  o.progress = progress;
  say("Indexing", label);
  const uint32_t t0 = millis();
  const bool ok = indexLibrary(o, s);
  Serial.printf("[index] %s took %lu ms (folders opened by cluster %lu, by path %lu)\n", label,
                millis() - t0, CardDir::clusterOpens, CardDir::pathOpens);
  CardDir::clusterOpens = CardDir::pathOpens = 0;
  printStats(label, s);
  return ok;
}

// ---------------------------------------------------------------- Integrity
static void lowerKey(char *dst, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n - 1 && src[i]; i++) dst[i] = tolower(static_cast<unsigned char>(src[i]));
  dst[i] = 0;
}

static uint32_t checkLibrary(uint32_t gen, bool printArtists) {
  uint32_t problems = 0;
  auto fail = [&](const char *what, uint32_t at) {
    if (problems < 10) Serial.printf("[index]   PROBLEM: %s at %lu\n", what, at);
    problems++;
  };
  if (!LibStore::verifyGeneration(gen)) fail("CRC verify", 0);

  LibReader songs, titleIdx, artistIdx, albumIdx, artists, albums;
  songs.open(gen, LF_SONGS, sizeof(SongRecord));
  titleIdx.open(gen, LF_TITLE_INDEX, sizeof(uint32_t));
  artistIdx.open(gen, LF_ARTIST_INDEX, sizeof(uint32_t));
  albumIdx.open(gen, LF_ALBUM_INDEX, sizeof(uint32_t));
  artists.open(gen, LF_ARTISTS, sizeof(ArtistRecord));
  albums.open(gen, LF_ALBUMS, sizeof(AlbumRecord));
  const uint32_t n = songs.count();
  std::vector<uint8_t> seen((n + 7) / 8);
  SongRecord r;
  uint32_t id;

  // title.idx: a permutation, in (bucket, lowercase title) order.
  if (titleIdx.count() != n) fail("title.idx count", titleIdx.count());
  std::fill(seen.begin(), seen.end(), 0);
  uint8_t prevBucket = 0;
  char prev[40] = "", cur[40];
  for (uint32_t i = 0; i < titleIdx.count(); i++) {
    titleIdx.read(i, &id);
    if (id >= n || (seen[id / 8] & (1 << (id % 8)))) { fail("title.idx id", i); continue; }
    seen[id / 8] |= 1 << (id % 8);
    songs.read(id, &r);
    lowerKey(cur, r.title, sizeof(cur));
    const uint8_t b = libBucket(r.title);
    if (b < prevBucket || (b == prevBucket && strcmp(cur, prev) < 0)) fail("title order", i);
    prevBucket = b;
    strcpy(prev, cur);
  }
  JumpTable jump;
  titleIdx.readExtra(&jump);
  for (int b = 0; b < 27; b++) if (jump.start[b] > jump.start[b + 1]) fail("title jump", b);
  if (jump.start[27] != n) fail("title jump total", jump.start[27]);
  for (int b = 1; b < 27; b++) {
    if (jump.start[b] < jump.start[b + 1]) {
      titleIdx.read(jump.start[b], &id);
      songs.read(id, &r);
      if (libBucket(r.title) != b) fail("title jump target", b);
    }
  }

  // artist.idx: a permutation; artists/albums partition it consistently.
  if (artistIdx.count() != n) fail("artist.idx count", artistIdx.count());
  std::fill(seen.begin(), seen.end(), 0);
  for (uint32_t i = 0; i < artistIdx.count(); i++) {
    artistIdx.read(i, &id);
    if (id >= n || (seen[id / 8] & (1 << (id % 8)))) { fail("artist.idx id", i); continue; }
    seen[id / 8] |= 1 << (id % 8);
  }
  ArtistRecord a;
  AlbumRecord al;
  uint32_t songSum = 0, albumSum = 0, expectPos = 0, printed = 0;
  for (uint32_t ai = 0; ai < artists.count(); ai++) {
    artists.read(ai, &a);
    if (a.firstSongPos != expectPos) fail("artist song range", ai);
    if (a.firstAlbum != albumSum) fail("artist album range", ai);
    uint32_t inAlbums = 0;
    String albumList;
    for (uint32_t k = a.firstAlbum; k < a.firstAlbum + a.albumCount; k++) {
      albums.read(k, &al);
      if (al.artistId != ai) fail("album artistId", k);
      if (al.firstSongPos != a.firstSongPos + inAlbums) fail("album song range", k);
      // Every song in the album carries that album name (unless unknown).
      for (uint32_t p = al.firstSongPos; p < al.firstSongPos + al.songCount; p++) {
        artistIdx.read(p, &id);
        songs.read(id, &r);
        if (strcmp(al.name, "Unknown Album") != 0 && strncasecmp(r.album, al.name, sizeof(al.name) - 1) != 0) {
          fail("album member", p);
        }
      }
      inAlbums += al.songCount;
      if (albumList.length() < 120) albumList += String(albumList.isEmpty() ? "" : "; ") + al.name + " (" + al.songCount + ")";
    }
    if (inAlbums != a.songCount) fail("artist album song sum", ai);
    if (printArtists && strncmp(a.name, "Artist ", 7) != 0 && printed < 40) {
      Serial.printf("[index]   %-28s %2lu album(s) %3lu song(s): %s\n", a.name, a.albumCount, a.songCount,
                    albumList.c_str());
      printed++;
    }
    expectPos += a.songCount;
    songSum += a.songCount;
    albumSum += a.albumCount;
  }
  if (songSum != n) fail("artist song total", songSum);
  if (albumSum != albums.count()) fail("album total", albumSum);
  artists.readExtra(&jump);
  if (jump.start[27] != artists.count()) fail("artist jump total", jump.start[27]);

  // album.idx: a permutation of album ids in name order.
  if (albumIdx.count() != albums.count()) fail("album.idx count", albumIdx.count());
  std::vector<uint8_t> seenAlbums((albums.count() + 7) / 8);
  prev[0] = 0;
  prevBucket = 0;
  for (uint32_t i = 0; i < albumIdx.count(); i++) {
    albumIdx.read(i, &id);
    if (id >= albums.count() || (seenAlbums[id / 8] & (1 << (id % 8)))) { fail("album.idx id", i); continue; }
    seenAlbums[id / 8] |= 1 << (id % 8);
    albums.read(id, &al);
    const bool unknown = strcmp(al.name, "Unknown Album") == 0;
    const uint8_t b = unknown ? 0xFF : libBucket(al.name);
    lowerKey(cur, al.name, 24);
    if (b < prevBucket || (b == prevBucket && strcmp(cur, prev) < 0)) fail("album order", i);
    prevBucket = b;
    strcpy(prev, cur);
  }
  return problems;
}

// Re-reads tags of random songs by path; compares with stored records.
static uint32_t sampleCheck(uint32_t gen, uint32_t samples) {
  LibReader songs, folders, names;
  songs.open(gen, LF_SONGS, sizeof(SongRecord));
  folders.open(gen, LF_FOLDERS, sizeof(FolderRecord));
  names.open(gen, LF_NAMES, 1);
  uint32_t bad = 0;
  char buf[512];
  for (uint32_t i = 0; i < samples; i++) {
    const uint32_t id = esp_random() % songs.count();
    SongRecord rec, again = {};
    FolderRecord f;
    songs.read(id, &rec);
    folders.read(rec.folderId, &f);
    names.readBytes(sizeof(LibHeader) + f.pathOffset, buf, f.pathLen);
    String path(buf, f.pathLen);
    names.readBytes(sizeof(LibHeader) + rec.nameOffset, buf, rec.nameLen);
    const String name(buf, rec.nameLen);
    path += "/" + name;
    CardFile card;
    TagInfo tags;
    if (card.open(path)) readTags(card, tags);
    libFillSong(again, tags, name.c_str());
    if (strcmp(again.title, rec.title) || strcmp(again.artist, rec.artist) ||
        again.durationMs != rec.durationMs || card.size() != rec.fileSize) {
      bad++;
      Serial.printf("[index]   MISMATCH %s\n", path.c_str());
    }
  }
  return bad;
}

static bool hasTitle(uint32_t gen, const char *title) {
  LibReader songs;
  songs.open(gen, LF_SONGS, sizeof(SongRecord));
  SongRecord r;
  for (uint32_t i = 0; i < songs.count(); i++) {
    songs.read(i, &r);
    if (strcmp(r.title, title) == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------- Scale run
#include "Catalog.h"

// Average/max ms to read one screen (6 rows) at random positions of a list,
// the way the UI does: index entry, then the record(s) behind it.
static void listLatency(const char *name, uint32_t count,
                        bool (*readRow)(uint32_t pos), uint32_t pages = 60) {
  if (count < 6) return;
  uint64_t total = 0;
  float worst = 0;
  for (uint32_t p = 0; p < pages; p++) {
    const uint32_t top = esp_random() % (count - 5);
    const uint32_t t0 = micros();
    for (uint32_t r = 0; r < 6; r++) readRow(top + r);
    const uint32_t us = micros() - t0;
    total += us;
    worst = max(worst, us / 1000.0f);
  }
  Serial.printf("[index]   %-8s screen of 6 rows: avg %.1f ms, max %.1f ms (%lu entries)\n", name,
                total / 1000.0f / pages, worst, count);
}

static bool songRowRead(uint32_t pos) {
  uint32_t id;
  Track t;
  return Catalog::titleSong(pos, id) && Catalog::song(id, t);
}

static bool artistRowRead(uint32_t pos) {
  ArtistRecord a;
  return Catalog::artist(pos, a);
}

static bool albumRowRead(uint32_t pos) {
  uint32_t id;
  AlbumRecord al;
  ArtistRecord a;
  return Catalog::albumByName(pos, id) && Catalog::album(id, al) && Catalog::artist(al.artistId, a);
}

// Rebuilds the same card state with a full sort (reusing tags) and checks
// every index file matches `patched` exactly (manifest data CRCs). The full
// build becomes the live generation; that's fine, it's identical.
// Afterwards patched.gen names the reference build (the patched one is gone).
static bool matchesFullSort(IndexStats &patched) {
  ManifestRecord a = {}, b = {};
  if (!patched.ok || !LibStore::validateGeneration(patched.gen, &a)) {
    return false;
  }
  IndexStats ref;
  runIndex("reference full sort", ref, false, 0, true);
  if (!ref.ok || !LibStore::validateGeneration(ref.gen, &b)) {
    return false;
  }
  patched.gen = ref.gen;
  bool same = true;
  for (int t = LF_NAMES; t < LF_TYPE_COUNT; t++) {
    if (t == LF_MANIFEST) {
      continue;
    }
    if (a.fileCrc[t] != b.fileCrc[t] || a.fileCount[t] != b.fileCount[t]) {
      Serial.printf("[index]   DIFFERENT from full sort: %s\n", kLibFileNames[t]);
      same = false;
    }
  }
  return same;
}

// RUN_BENCH 14: one full sort of the current card (tags reused) with the
// phase breakdown, for profiling the sort.
void runSortProfile() {
  Serial.println();
  Serial.println("[index] ===== sort profile =====");
  // Wait (up to 60 s) for a line on serial, so the monitor is attached first.
  for (uint32_t t0 = millis(); !Serial.available() && millis() - t0 < 60000;) {
    delay(10);
  }
  while (Serial.available()) {
    Serial.read();
  }
  if (!libraryMountCard()) {
    Serial.println("[index] SD mount failed");
    return;
  }
  // Tag reading on real songs (not the generated ones): no cache vs block caches.
  {
    const uint32_t gen = LibStore::findCurrent();
    std::vector<String> paths;
    Catalog::open(gen);
    for (uint32_t id = 0; id < Catalog::songCount() && paths.size() < 300; id++) {
      const String p = Catalog::songPath(id);
      if (!p.startsWith("/Big Test/") && !p.startsWith("/_bench/")) {
        paths.push_back(p);
      }
    }
    Catalog::close();
    Serial.printf("[index] tag profile over %u real songs\n", paths.size());
    uint32_t refHash = 0;
    for (uint32_t cache : {0u, 2048u, 4096u, 8192u, 0u}) {
      CardFile card;
      card.setCache(cache);
      uint32_t us = 0, opened = 0, hash = 0;
      CardFile::readCalls = CardFile::cardReads = 0;
      for (const String &p : paths) {
        if (!card.open(p)) {
          continue;
        }
        opened++;
        TagInfo t;
        const uint32_t t0 = micros();
        readTags(card, t);
        us += micros() - t0;
        card.close();
        const String all = t.title + t.artist + t.album + t.albumArtist + String(t.durationMs) +
                           String(t.audioStart) + String(t.track);
        hash = esp_rom_crc32_le(hash, reinterpret_cast<const uint8_t *>(all.c_str()), all.length());
      }
      if (refHash == 0) {
        refHash = hash;
      }
      Serial.printf("[index]   cache %5lu: %.2f ms/song tags, %.1f reads/song (%.1f card reads), "
                    "results %s\n",
                    cache, opened ? us / 1000.0f / opened : 0.0f,
                    opened ? static_cast<float>(CardFile::readCalls) / opened : 0.0f,
                    opened ? static_cast<float>(CardFile::cardReads) / opened : 0.0f,
                    hash == refHash ? "same" : "DIFFERENT");
    }
  }
#if 0  // full sort profile
  IndexStats s;
  runIndex("full sort", s, false, 0, true);
#endif
  Serial.println("[index] DONE");
}

void runScaleBench() {
  Serial.println();
  Serial.println("[index] ===== step 7: scale =====");
  for (uint32_t t0 = millis(); !Serial.available() && millis() - t0 < 60000;) {
    delay(10);  // wait for a line on serial (monitor attached), up to 60 s
  }
  while (Serial.available()) {
    Serial.read();
  }
  if (!libraryMountCard()) {
    Serial.println("[index] SD mount failed: power-cycle the board (card stuck)");
    return;
  }
  Serial.printf("[index] free heap at start %u\n", ESP.getFreeHeap());

  IndexStats full;
  const bool ok1 = runIndex("full index", full, true);
  uint32_t t0 = millis();
  const uint32_t problems = checkLibrary(full.gen, false);
  const uint32_t checkMs = millis() - t0;
  const uint32_t bad = sampleCheck(full.gen, 100);
  Serial.printf("[index] 1 full index of %lu songs: %s; integrity problems %lu (check took %lu s); "
                "100-song re-read mismatches %lu -> %s\n",
                full.songs, ok1 ? "ok" : "FAILED", problems, checkMs / 1000, bad,
                passFail(ok1 && problems == 0 && bad == 0 && full.failedOpens == 0));

  IndexStats same;
  runIndex("unchanged check", same, false);
  Serial.printf("[index] 2 unchanged check: detected=%d in %lu ms -> %s\n", same.unchanged,
                same.checkMs, passFail(same.ok && same.unchanged));

  // 3. Small changes: each should patch the previous sort keys, and give
  //    exactly the indexes a full sort of the same card gives.
  const String bigRoot = SD.exists("/Big Test") ? "/Big Test" : "/_bench/big";
  const String added = bigRoot + "/Zz Added Song.mp3";
  benchWriteSong(added, "Zz Added Song", 1);
  IndexStats plusOne;
  runIndex("one song added", plusOne, false);
  const bool addOk = plusOne.ok && plusOne.tagged == 1 && plusOne.songs == full.songs + 1 &&
                     hasTitle(plusOne.gen, "Zz Added Song");
  const bool addSame = matchesFullSort(plusOne);

  // Rewrite one GENERATED song (never a real one): the 1000th under bigRoot.
  String retagPath;
  Catalog::open(plusOne.gen);
  for (uint32_t id = 0, seen = 0; id < Catalog::songCount(); id++) {
    const String p = Catalog::songPath(id);
    if (p.startsWith(bigRoot + "/") && p.indexOf("Zz ") < 0 && ++seen == 1000) {
      retagPath = p;
      break;
    }
  }
  Catalog::close();
  IndexStats retag;
  bool retagOk = false, retagSame = false;
  if (retagPath.length()) {
    delay(2100);  // FAT times have 2 s resolution
    benchWriteSong(retagPath, "Zz Retagged Song", 7);
    runIndex("one song retagged", retag, false);
    retagOk = retag.ok && retag.tagged == 1 && retag.songs == plusOne.songs &&
              hasTitle(retag.gen, "Zz Retagged Song");
    retagSame = matchesFullSort(retag);
  } else {
    Serial.println("[index]   no generated song found to retag");
  }

  SD.remove(added);
  IndexStats minusOne;
  runIndex("one song removed", minusOne, false);
  const bool removeOk = minusOne.ok && minusOne.songs == full.songs &&
                        !hasTitle(minusOne.gen, "Zz Added Song");
  const bool removeSame = matchesFullSort(minusOne);
  const uint32_t problems3 = checkLibrary(minusOne.gen, false);
  auto secs = [](const IndexStats &s) {
    return (s.checkMs + s.walkMs + s.songsMs + s.sortMs + s.commitMs) / 1000;
  };
  Serial.printf("[index] 3 added %lu s (patched %d, same as full sort %d), retagged %s %lu s "
                "(patched %d, same %d), removed %lu s (patched %d, same %d); integrity problems "
                "%lu -> %s\n",
                secs(plusOne), plusOne.sortPatched, addSame, retagPath.c_str(), secs(retag),
                retag.sortPatched, retagSame, secs(minusOne), minusOne.sortPatched, removeSame,
                problems3,
                passFail(addOk && retagOk && removeOk && addSame && retagSame && removeSame &&
                         plusOne.sortPatched && retag.sortPatched && minusOne.sortPatched &&
                         problems3 == 0));

  Catalog::open(minusOne.gen);
  Serial.println("[index] 4 list reads at scale:");
  listLatency("Songs", Catalog::songCount(), songRowRead);
  listLatency("Artists", Catalog::artistCount(), artistRowRead);
  listLatency("Albums", Catalog::albumCount(), albumRowRead);
  t0 = micros();
  const JumpTable *j = Catalog::jump(JUMP_TITLES);
  Serial.printf("[index]   jump table load %.1f ms; songs under M: %lu\n", (micros() - t0) / 1000.0f,
                j ? j->start[14] - j->start[13] : 0);
  Catalog::close();

  Serial.printf("[index] free heap %u, min free heap %u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  Serial.println("[index] DONE");
  say("Scale test done", "Results on serial");
}

void runStep3Bench() {
  Serial.println();
  Serial.println("[index] ===== step 3: indexer =====");
  for (uint32_t t0 = millis(); !Serial.available() && millis() - t0 < 60000;) {
    delay(10);  // wait for a line on serial (monitor attached), up to 60 s
  }
  while (Serial.available()) {
    Serial.read();
  }
  libraryMountCard();

  // 1. Full index (all tags read; fast opens).
  IndexStats full;
  const bool t1 = runIndex("full index", full, true);
  const uint32_t problems = checkLibrary(full.gen, true);
  const uint32_t bad = sampleCheck(full.gen, 80);
  Serial.printf("[index] 1 full index: %s; integrity problems %lu; 80-song re-read mismatches %lu; "
                "fast opens %lu / path opens %lu -> %s\n",
                t1 ? "ok" : "FAILED", problems, bad, full.fastOpens, full.pathOpens,
                // A few entries can't be fast-opened (their directory sector
                // was already evicted); they fall back to a normal open.
                passFail(t1 && problems == 0 && bad == 0 && full.failedOpens == 0 &&
                         full.pathOpens * 20 <= full.songs));

  // 2. Nothing changed.
  IndexStats same;
  runIndex("unchanged re-run", same, false);
  Serial.printf("[index] 2 unchanged: detected=%d, %lu ms -> %s\n", same.unchanged, same.checkMs,
                passFail(same.ok && same.unchanged && same.gen == full.gen));

  // 3. Add one, delete one, retag one.
  const String added = "/_bench/lib/d00/Bench Song NEW.mp3";
  const String deleted = benchSongPath(1, 5);   // "Bench Song 105"
  const String retagged = benchSongPath(2, 7);  // "Bench Song 207"
  benchWriteSong(added, "Bench Song NEW", 9999);
  SD.remove(deleted);
  delay(2100);  // FAT times have 2 s resolution; make sure the rewrite changes mtime
  benchWriteSong(retagged, "Retagged Song", 207);
  IndexStats changed;
  runIndex("after add/delete/retag", changed, false);
  const bool seen3 = hasTitle(changed.gen, "Bench Song NEW") && hasTitle(changed.gen, "Retagged Song") &&
                     !hasTitle(changed.gen, "Bench Song 105") && !hasTitle(changed.gen, "Bench Song 207");
  const uint32_t problems3 = checkLibrary(changed.gen, false);
  const bool same3 = matchesFullSort(changed);
  Serial.printf("[index] 3 add/delete/retag: songs %lu (was %lu), tagged %lu (expect 2), reused %lu, "
                "titles as expected %d, integrity problems %lu, sort patched %d, same as full "
                "sort %d -> %s\n",
                changed.songs, full.songs, changed.tagged, changed.reused, seen3, problems3,
                changed.sortPatched, same3,
                passFail(changed.ok && changed.tagged == 2 && seen3 && problems3 == 0 &&
                         changed.songs == full.songs && changed.sortPatched && same3));
  // Put the test files back.
  SD.remove(added);
  benchWriteSong(deleted, "Bench Song 105", 105);
  benchWriteSong(retagged, "Bench Song 207", 207);
  IndexStats restored;
  runIndex("restore", restored, false);

  // 4. Crash mid-build, then resume.
  IndexStats crashed, resumed;
  const bool crashOk = runIndex("crash after 700 songs", crashed, true, 700);
  runIndex("resume", resumed, true);
  const uint32_t problems4 = checkLibrary(resumed.gen, false);
  Serial.printf("[index] 4 crash+resume: crash run returned %d (%s); resumed=%d at %lu, tagged %lu "
                "(expect %lu), integrity problems %lu -> %s\n",
                crashOk, crashed.error.c_str(), resumed.resumed, resumed.resumedAt, resumed.tagged,
                full.songs - resumed.resumedAt, problems4,
                passFail(!crashOk && resumed.ok && resumed.resumed && resumed.resumedAt == 512 /* last checkpoint (every 256) before 700 */ &&
                         resumed.tagged == full.songs - 512 && problems4 == 0 &&
                         resumed.songs == full.songs));

  Serial.printf("[index] free heap %u, min free heap %u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  Serial.println("[index] DONE");
  say("Index test done", "Results on serial");
}

#endif  // RUN_BENCH == 4 || RUN_BENCH == 10 || RUN_BENCH == 14
