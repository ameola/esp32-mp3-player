#pragma once
// Card mounting and background library indexing.
//
// The indexer (Indexer.h) runs as a low-priority FreeRTOS task on core 0
// while the UI and audio keep running on core 1. FatFs (reentrant build) and
// the Arduino SPI bus lock serialize their card access. The task never
// deletes the generation the UI has open: the main loop switches the Catalog
// to the new generation first, then removes the old one.

#include <Arduino.h>

#include "Indexer.h"

// Mounts the SD card. Returns false if no card.
bool libraryMountCard();

// Starts indexing in the background (no-op if already running). fullRebuild
// re-reads every song's tags instead of reusing unchanged ones.
void libraryStartIndexing(bool fullRebuild = false);
bool libraryIndexing();
bool libraryFullRebuild();  // the running (or last) run is a full rebuild

struct LibraryProgress {
  const char *phase;   // e.g. "Reading tags"
  uint32_t done;
  uint32_t total;      // 0 when unknown
  char detail[96];     // folder being worked on ("" if none)
};

// Latest progress reported by the running indexer.
void libraryProgress(LibraryProgress &out);

// True once, when a background run has finished; fills its stats.
bool libraryTakeResult(IndexStats &stats);

// True for folder names the scanner skips (hidden, system, Bench data).
bool librarySkipFolder(const char *name);
bool libraryIsMp3Name(const char *name);
