#include "Library.h"

#include <SD.h>
#include <new>
#include <SPI.h>

#include "CardFile.h"
#include "Config.h"

bool libraryIsMp3Name(const char *name) {
  if (name[0] == '.') {  // skips macOS "._" resource files too
    return false;
  }
  const size_t len = strlen(name);
  return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
}

bool librarySkipFolder(const char *name) {
  // "_bench" holds Bench's 2000 synthetic files (kept for indexer tests).
  return name[0] == '.' || strcmp(name, "_bench") == 0 ||
         strcmp(name, "System Volume Information") == 0;
}

bool libraryMountCard() {
  // A reset of the ESP32 doesn't reset the card: one cut off mid-transfer can
  // refuse the first init. Retrying (fresh init clocks + CMD0) usually clears it.
  for (int attempt = 1; attempt <= 3; attempt++) {
    SD.end();
    // max_files: the VFS layer reserves a 4 KB FatFs file object per slot at
    // mount. The player opens card files through FatFs directly (CardFile.h),
    // so one slot is enough.
    if (SD.begin(SD_CS, SPI, SD_SPI_HZ, "/sd", 1)) {
      cardNoteMount();
      if (attempt > 1) {
        Serial.printf("SD mounted on attempt %d\n", attempt);
      }
      return true;
    }
    delay(100);
  }
  Serial.println("SD mount failed");
  return false;
}

// ================================================================ Background task
static volatile bool running = false;
static volatile bool finished = false;
static bool fullRebuild = false;
static IndexStats result;
static portMUX_TYPE progressLock = portMUX_INITIALIZER_UNLOCKED;
static LibraryProgress progress = {"", 0, 0, ""};

// Runs on the indexing task: copy everything the UI may read later.
static void onProgress(const char *phase, uint32_t done, uint32_t total, const char *detail) {
  char shortened[sizeof(progress.detail)];
  shortened[0] = 0;
  if (detail) {
    const size_t len = strlen(detail);
    const size_t keep = sizeof(shortened) - 1;
    // Keep the end of long paths (the most specific folders).
    strlcpy(shortened, len > keep ? detail + (len - keep) : detail, sizeof(shortened));
  }
  portENTER_CRITICAL(&progressLock);
  progress.phase = phase;
  progress.done = done;
  progress.total = total;
  if (detail) {
    memcpy(progress.detail, shortened, sizeof(shortened));
  }
  portEXIT_CRITICAL(&progressLock);
}

static void indexTask(void *) {
  cardSetYieldTask(xTaskGetCurrentTaskHandle());
  IndexOptions options;
  options.removeOld = false;  // the UI may still be reading it
  options.forceFull = fullRebuild;
  options.progress = onProgress;
  IndexStats stats;
  const uint32_t t0 = millis();
  // The shared SPI bus sees occasional CRC errors (logged by the SD driver
  // as "sdCommand(): crc error"); the driver retries, but now and then a
  // write still fails (FR_DISK_ERR). A failed build is simply run again: it
  // resumes from its last checkpoint (phase B) or redoes the cheap phases.
  for (int attempt = 1; attempt <= 3; attempt++) {
    // Out of memory anywhere in the build (e.g. a std::vector growing while
    // playback has fragmented the heap) fails this attempt instead of
    // aborting the player. Destructors close the files on the way out.
    try {
      indexLibrary(options, stats);
    } catch (const std::bad_alloc &) {
      stats.ok = false;
      portENTER_CRITICAL(&progressLock);
      const char *phase = progress.phase;
      portEXIT_CRITICAL(&progressLock);
      stats.error = String("out of memory (") + phase + ", largest block " +
                    String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + ")";
    }
    if (stats.ok) {
      break;
    }
    Serial.printf("Library build attempt %d failed: %s\n", attempt, stats.error.c_str());
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  Serial.printf("Library gen %lu: %lu songs, %lu artists, %lu albums, %lu folders; %s in %lu ms "
                "(check %lu, walk %lu, songs %lu, sort %lu, commit %lu ms; reused %lu, tagged %lu; "
                "min heap %u)%s%s\n",
                stats.gen, stats.songs, stats.artists, stats.albums, stats.folders,
                !stats.ok ? "FAILED" : stats.unchanged ? "unchanged" : "rebuilt", millis() - t0,
                stats.checkMs, stats.walkMs, stats.songsMs, stats.sortMs, stats.commitMs,
                stats.reused, stats.tagged, ESP.getMinFreeHeap(),
                stats.error.isEmpty() ? "" : ": ", stats.error.c_str());
  if (stats.ok && !stats.unchanged) {
    Serial.printf("  sort %s%s%s (kept %lu songs)\n", stats.sortPatched ? "patched" : "full",
                  stats.patchSkipped.isEmpty() ? "" : ": ", stats.patchSkipped.c_str(),
                  stats.keptSongs);
    Serial.printf("  tags %lu ms of songs; sort = feed %lu + title idx %lu + artists %lu + "
                  "album idx %lu ms; %lu runs, sort buffers %lu KB\n",
                  stats.tagMs, stats.feedMs, stats.titleIdxMs, stats.artistIdxMs,
                  stats.albumIdxMs, stats.sortRuns, stats.sortBufferBytes / 1024);
  }
  result = stats;
  finished = true;
  running = false;
  vTaskDelete(nullptr);
}

void libraryStartIndexing(bool full) {
  if (running) {
    return;
  }
  running = true;
  finished = false;
  fullRebuild = full;
  portENTER_CRITICAL(&progressLock);
  progress.phase = full ? "Scanning folders" : "Checking card";
  progress.done = progress.total = 0;
  progress.detail[0] = 0;
  portEXIT_CRITICAL(&progressLock);
  // Core 0, priority 1: above that core's idle task, so it gets the whole
  // core (at priority 0 it was time-sliced with idle, ~2x slower); it sleeps
  // a tick every 50 ms (cardYieldPoint) so idle still feeds the watchdog.
  // The UI runs on core 1. Audio shares core 0 at a higher priority (see
  // AudioPlayer), so playback always gets the CPU and the card first.
  xTaskCreatePinnedToCore(indexTask, "indexer", 16384, nullptr, 1, nullptr, 0);
}

bool libraryIndexing() {
  return running;
}

bool libraryFullRebuild() {
  return fullRebuild;
}

void libraryProgress(LibraryProgress &out) {
  portENTER_CRITICAL(&progressLock);
  out = progress;
  portEXIT_CRITICAL(&progressLock);
}

bool libraryTakeResult(IndexStats &stats) {
  if (!finished) {
    return false;
  }
  finished = false;
  stats = result;
  return true;
}
