#pragma once
// Builds the on-card library (LibFormat.h) from the card's MP3 files.
//
// Phases, all streaming so RAM use doesn't grow with library size:
//   0. Check   read-only pass over the card computing a fingerprint of the
//              MP3 listing (path, size, time of every file). If it equals the
//              live library's, nothing changed and indexing stops here.
//   A. Walk    folders breadth-first with FatFs f_readdir (pending folders
//              queue in a temp file). Writes folders.dat, names.dat and a
//              skeleton (one entry per song: folder, name, size, time, start
//              cluster), plus the same fingerprint.
//   B. Songs   one SongRecord per skeleton entry. Unchanged files (same name,
//              size, time in the same folder of the live library) are copied;
//              others get their tags read, opening files by start cluster
//              (no name lookup). Checkpoints every 64 songs, so an interrupted
//              build resumes where it stopped after a reboot.
//   C. Sort    on-card merge sorts build title.idx, artist.idx, artists.dat,
//              albums.dat and album.idx. Albums with no album artist and
//              several track artists in one folder are filed under
//              "Various Artists".
//   D. Commit  read-back verify, manifest, delete the old generation.

#include <Arduino.h>

struct IndexOptions {
  bool includeBench = false;     // also index /_bench (test files)
  bool forceFull = false;        // ignore the live library (re-read every tag)
  bool removeOld = true;         // delete the previous generation after commit
                                 // (false when another task still reads it)
  uint32_t stopAfterSongs = 0;   // test hook: simulate a crash in phase B
  bool fullSort = false;         // test hook: skip the unchanged check and the
                                 // sort-key patch (reference build to compare)
  // Called regularly with the current phase, progress and detail (the folder
  // being worked on, or nullptr). May be nullptr. Called from the indexing
  // task; copy what you need.
  void (*progress)(const char *phase, uint32_t done, uint32_t total, const char *detail) = nullptr;
};

struct IndexStats {
  bool ok = false;
  bool unchanged = false;        // listing matched the live library
  bool resumed = false;
  uint32_t resumedAt = 0;
  uint32_t gen = 0;              // live generation afterwards
  uint32_t songs = 0, folders = 0, artists = 0, albums = 0;
  uint32_t reused = 0, tagged = 0;
  uint32_t fastOpens = 0, pathOpens = 0, failedOpens = 0;
  uint32_t checkMs = 0, walkMs = 0, songsMs = 0, sortMs = 0, commitMs = 0;
  uint32_t sortRuns = 0;
  // Breakdown: time inside song phase spent opening files + reading tags;
  // sort phase split into feeding the sorters and writing each index.
  uint32_t tagMs = 0, feedMs = 0, titleIdxMs = 0, artistIdxMs = 0, albumIdxMs = 0;
  uint32_t sortBufferBytes = 0;
  uint32_t feedReadMs = 0, feedAddMs = 0;  // feed: reading songs / adding (incl. spills)
  uint32_t largestBlockAtSort = 0;
  bool sortPatched = false;
  String patchSkipped;        // why the patch wasn't used ("" if it was, or not tried)   // indexes merged from the previous build's sort keys
  uint32_t keptSongs = 0;     // songs in unchanged folders (patch mode)
  uint32_t minFreeHeap = 0;
  String error;
};

bool indexLibrary(const IndexOptions &options, IndexStats &stats);
