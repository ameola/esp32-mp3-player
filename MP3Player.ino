// Simple touchscreen MP3 player for the ELEGOO ESP32-WROOM-32.
// Display: ILI9341V 240x320 (portrait). Touch: FT6336U (polled over I2C).
// Storage: microSD on the LCD board (shared SPI). Audio: MAX98357A over I2S.
//
// Modules:
//   Config.h       pins, tuning, layout, colors
//   Lcd            raw-SPI display driver + off-screen canvas
//   Touch          FT6336U polling
//   AudioPlayer    MP3 decoding and always-on I2S output
//   Text           character conversion for the ASCII font
//   Tags           ID3 tag reader
//   Library        song list (to be replaced by the on-card database)
//   Ui             screen drawing and touch handling
//   Bench          SD / indexing benchmarks (build with -DRUN_BENCH)
//
// Requires: Adafruit GFX Library, Adafruit ILI9341, ESP8266Audio (2.x).

#include <esp_heap_caps.h>

#include "ArtScan.h"
#include "ArtFetch.h"
#include "AudioPlayer.h"
#include "Catalog.h"
#include "Config.h"
#include "Lcd.h"
#include "LibStore.h"
#include "Library.h"
#include "Touch.h"
#include "Ui.h"
#include "UiTheme.h"
#include "CardSettings.h"
#include "WifiSetup.h"

#ifdef RUN_BENCH
#include "Bench.h"
#endif

// The library code nests folder walks and file objects; 8 KB (the default)
// is tight.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

enum AppState {
  NO_CARD,        // waiting for a card (tap to retry)
  WIFI_SETUP,     // card not set up yet: asking for WiFi details (album art)
  FIRST_INDEX,    // no library yet: progress screen until it's built
  REBUILD,        // full rebuild progress screen (tap to return to the player)
  PLAYER,         // library open; the card may be re-checked in the background
};
static AppState appState = NO_CARD;


// Free heap after each startup step, kept for the "mem" command (the serial
// monitor usually attaches after boot).
static String memLog;
static void memStep(const char *step, bool keep = true) {
  char line[100];
  snprintf(line, sizeof(line), "[mem] %-14s free %6u, byte-addressable %6u, largest %6u\n", step,
           ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_8BIT), ESP.getMaxAllocHeap());
  if (keep) memLog += line;
  Serial.print(line);
}
static uint32_t librarySongsBefore = 0;  // to estimate folder-scan progress

static String withCommas(uint32_t n) {
  String digits(n);
  String out;
  for (int i = 0; i < static_cast<int>(digits.length()); i++) {
    if (i > 0 && (digits.length() - i) % 3 == 0) {
      out += ',';
    }
    out += digits[i];
  }
  return out;
}

// Overall progress of a build across its phases (rough weights by time).
static float overallFraction(const LibraryProgress &p) {
  const float part = p.total ? static_cast<float>(p.done) / p.total : 0.0f;
  if (strcmp(p.phase, "Checking card") == 0) {
    return 0.0f;
  }
  if (strcmp(p.phase, "Scanning folders") == 0) {
    // Song count unknown until the scan ends: estimate from the old library.
    const float est = librarySongsBefore ? static_cast<float>(p.done) / librarySongsBefore : 0.0f;
    return 0.08f * min(est, 1.0f);
  }
  if (strcmp(p.phase, "Reading tags") == 0) return 0.08f + 0.77f * part;
  if (strcmp(p.phase, "Sorting") == 0) return 0.85f + 0.12f * part;
  if (strcmp(p.phase, "Writing indexes") == 0) return 0.97f;
  if (strcmp(p.phase, "Saving") == 0) return 0.98f;
  return 0.0f;
}

static void showBuildProgress(bool redraw) {
  LibraryProgress p;
  libraryProgress(p);
  String counts;
  if (strcmp(p.phase, "Scanning folders") == 0 || strcmp(p.phase, "Checking card") == 0) {
    counts = withCommas(p.done) + " songs found";
  } else if (strcmp(p.phase, "Reading tags") == 0) {
    counts = withCommas(p.done) + " / " + withCommas(p.total) + " songs";
  }
  const bool first = appState == FIRST_INDEX;
  Serial.printf("[progress] %s %d%% %s | %s\n", p.phase, static_cast<int>(overallFraction(p) * 100),
                counts.c_str(), p.detail);
  uiShowProgress(redraw, first ? "Building library" : "Rebuilding library", p.phase,
                 overallFraction(p), counts, p.detail,
                 first ? "This happens once. Later updates\nrun in the background."
                       : "Music keeps playing.\nTap anywhere to return to the player.");
}

// Two short lines for the tab bar corner, e.g. "Tags\n45%".
static String compactProgress() {
  LibraryProgress p;
  libraryProgress(p);
  static const char *const names[][2] = {
    {"Checking card", "Check"}, {"Scanning folders", "Scan"}, {"Reading tags", "Tags"},
    {"Sorting", "Sort"},        {"Writing indexes", "Index"}, {"Saving", "Save"},
  };
  String text = "Update";
  for (const auto &n : names) {
    if (strcmp(p.phase, n[0]) == 0) {
      text = n[1];
    }
  }
  if (libraryFullRebuild()) {
    text = String(static_cast<int>(overallFraction(p) * 100)) + "%";
    return "Rebuild\n" + text;
  }
  if (p.total > 0) {
    text += "\n" + String(p.done * 100 / p.total) + "%";
  } else if (p.done > 0) {
    text += "\n" + String(p.done);
  }
  return text;
}

// Opens the live library, if there is one, and shows the player right away;
// then checks the card for changes in the background.
static void startLibrary() {
  if (!libraryMountCard()) {
    appState = NO_CARD;
    uiShowMessage("No SD card", "Insert a card and tap the screen");
    return;
  }
  memStep("card mounted");
  CardSettings settings;
  if (!cardSettingsLoad(settings)) {  // a card the player hasn't set up yet
    appState = WIFI_SETUP;
    wifiSetupBegin();
    return;
  }
  artFetchSetEnabled(settings.artDownload);
  artFetchStop();  // the library update needs the memory (and may replace the files)
  const uint32_t live = LibStore::findCurrent();
  if (live != LIB_NONE && Catalog::open(live)) {
    memStep("catalog open");
    appState = PLAYER;
    uiShowPlayer();
    uiSetLibraryStatus("Check");
  } else {
    appState = FIRST_INDEX;
    librarySongsBefore = 0;
    showBuildProgress(true);
  }
  libraryStartIndexing();
}

static void startFullRebuild() {
  if (libraryIndexing()) {
    return;  // an update is already running; it shows in the corner
  }
  librarySongsBefore = Catalog::songCount();
  artFetchStop();
  libraryStartIndexing(true);
  appState = REBUILD;
  showBuildProgress(true);
}

// Progress display and switching to a newly built library.
static void serviceLibrary() {
  static uint32_t lastDraw = 0;
  if (libraryIndexing() && millis() - lastDraw > 500) {
    lastDraw = millis();
    if (appState == FIRST_INDEX || appState == REBUILD) {
      showBuildProgress(false);
    } else if (appState == PLAYER) {
      uiSetLibraryStatus(compactProgress());
    }
  }

  IndexStats stats;
  if (!libraryTakeResult(stats)) {
    return;
  }
  if (!stats.ok) {
    if (appState == FIRST_INDEX) {
      uiShowMessage("Library error", stats.error.c_str());
      return;
    }
    if (appState == REBUILD) {
      appState = PLAYER;
      uiShowPlayer();
    }
    uiSetLibraryStatus("Update\nfailed");
    return;
  }
  const bool switched = stats.gen != Catalog::generation() && Catalog::open(stats.gen);
  if (switched) {
    LibStore::removeAllExcept(stats.gen);  // safe now: nothing reads the old one
  }
  uiSetLibraryStatus("");
  if (appState == FIRST_INDEX) {
    appState = PLAYER;
    uiShowPlayer();
  } else if (appState == REBUILD) {
    appState = PLAYER;
    uiLibraryChanged();  // redraws the player
  } else if (switched) {
    uiLibraryChanged();
  }
}

static void handleTouch(TouchPhase phase, int16_t x, int16_t y) {
  switch (appState) {
    case NO_CARD:
      if (phase == TOUCH_DOWN) {
        uiShowMessage("Reading card...", "");
        startLibrary();
      }
      break;
    case REBUILD:
      if (phase == TOUCH_DOWN) {  // back to the player; the rebuild continues
        appState = PLAYER;
        uiShowPlayer();
        uiSetLibraryStatus(compactProgress());
      }
      break;
    case PLAYER:
      uiHandleTouch(phase, x, y);
      break;
    case WIFI_SETUP:
      wifiSetupTouch(phase, x, y);
      break;
    default:
      break;
  }
}

// Debug aid: inject touches over Serial: "tap x y", "drag x y1 y2 [ms]",
// "audiocpu" (time spent decoding over 3 s), "artscan" (album art survey),
// "wifisetup" (WiFi setup screen), "artfetch" / "artstop" (album art pass), "artartists" (its artist steps only), "libdump" (every song's cleaned tags and path),
// "mem" (free memory after each startup step, and now),
// "hold x y ms". Prints the time taken and free heap afterwards.
static void serviceSerialCommands() {
  static String line;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c != '\n' && c != '\r') {
      line += c;
      continue;
    }
    int x = 0, y = 0, a = 0;
    const uint32_t t0 = millis();
    bool done = true;
    if (sscanf(line.c_str(), "tap %d %d", &x, &y) == 2) {
      handleTouch(TOUCH_DOWN, x, y);
      handleTouch(TOUCH_UP, x, y);
    } else if (sscanf(line.c_str(), "drag %d %d %d", &x, &y, &a) == 3) {
      // Optional 4th value: duration in ms (moves every 25 ms like the touch
      // controller, with the UI running in between; short = a flick).
      int ms = 0;
      sscanf(line.c_str(), "drag %*d %*d %*d %d", &ms);
      handleTouch(TOUCH_DOWN, x, y);
      if (ms <= 0) {
        for (int step = 1; step <= 8; step++) {
          handleTouch(TOUCH_MOVE, x, y + (a - y) * step / 8);
        }
      } else {
        // The finger moves at constant speed; samples every 25 ms or as
        // often as the UI allows, like the real touch controller.
        // The finger lifts at `ms` while still moving.
        const uint32_t start = millis();
        for (;;) {
          const uint32_t next = millis() + 25;
          while (static_cast<int32_t>(millis() - next) < 0) {
            if (appState == PLAYER) uiTick();
          }
          const uint32_t elapsed = millis() - start;
          if (elapsed >= static_cast<uint32_t>(ms)) break;
          handleTouch(TOUCH_MOVE, x, y + (a - y) * static_cast<int32_t>(elapsed) / ms);
        }
      }
      handleTouch(TOUCH_UP, x, a);
    } else if (sscanf(line.c_str(), "hold %d %d %d", &x, &y, &a) == 3) {
      handleTouch(TOUCH_DOWN, x, y);
      while (millis() - t0 < static_cast<uint32_t>(a)) {
        if (appState == PLAYER) {
          uiTick();
        }
        delay(10);
      }
      handleTouch(TOUCH_UP, x, y);
    } else if (line == "libdump") {  // every song: id, tags, path (tab-separated)
      Track t;
      const uint32_t n = Catalog::songCount();
      Serial.printf("[dump] begin %lu\n", n);
      for (uint32_t id = 0; id < n; id++) {
        if (!Catalog::song(id, t)) continue;
        Serial.printf("[dump]\t%lu\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%lu\t%s\n", id, t.title.c_str(), t.artist.c_str(),
                      t.album.c_str(), t.albumArtist.c_str(), t.track, t.disc, t.year, t.durationMs,
                      Catalog::songPath(id).c_str());
        if (id % 50 == 0) delay(1);
      }
      Serial.println("[dump] end");
    } else if (line == "mem") {
      Serial.print(memLog);
      memStep("now", false);
      Serial.printf("[mem] loop task stack unused: %u of 16384 bytes\n", uxTaskGetStackHighWaterMark(nullptr));
    } else if (line == "wifisetup") {  // show the setup screen again
      artFetchStop();
      appState = WIFI_SETUP;
      wifiSetupBegin();
    } else if (line == "artartists") {  // test: a pass of the artist steps only
      artFetchArtistsNow();
    } else if (line == "artfetch") {  // start an album art pass now
      artFetchNow();
    } else if (line == "artstop") {
      artFetchStop();
    } else if (line == "artscan") {
      artScanStart();
    } else if (line == "audiocpu") {
      extern uint32_t audioDecodeUs;
      audioDecodeUs = 0;
      const uint32_t under0 = audioUnderruns();
      const uint32_t start = millis();
      while (millis() - start < 3000) {
        if (appState == PLAYER) uiTick();
        delay(1);
      }
      extern void audioReadStats(uint32_t &, uint32_t &);
      uint32_t lowWater, starved;
      audioReadStats(lowWater, starved);
      Serial.printf("[cmd] audio decode: %lu ms of 3000 ms, underruns %lu, read-ahead low %lu bytes, "
                    "waits for card %lu\n",
                    audioDecodeUs / 1000, audioUnderruns() - under0, lowWater, starved);
    } else {
      done = false;
    }
    if (done) {
      Serial.printf("[cmd] %s: %lu ms, heap %u (min %u), underruns %lu\n", line.c_str(),
                    millis() - t0, ESP.getFreeHeap(), ESP.getMinFreeHeap(), audioUnderruns());
    }
    line = "";
  }
}



void setup() {
  const uint32_t heapAtStart = ESP.getFreeHeap();
  audioInit();  // first, so the amp's inputs are driven before anything else
  Serial.begin(115200);
  memLog = "[mem] setup start    free " + String(heapAtStart) + "\n";
  memStep("audio");
  uiStyleLoad();  // theme and layout settings, before anything is drawn
  lcdInit();
  memStep("lcd");
  touchInit();
  memStep("touch");

#ifdef RUN_BENCH
  runBench();
  return;
#endif

  const uint32_t t0 = millis();
  artFetchInit();
  audioSetBeforeRestore([] {  // play pressed during an art download: say why it waits
    if (artFetchRunning()) {
      uiSetLibraryStatus("Art\nstop");
      artFetchStop();
      uiSetLibraryStatus("");
    }
  });
  startLibrary();
  memStep("library open");
  Serial.printf("Ready (library shown %lu ms after display init)\n", millis() - t0);
}

void loop() {
#ifdef RUN_BENCH
  delay(1000);
  return;
#endif

  serviceLibrary();
  serviceSerialCommands();

  if (appState == WIFI_SETUP) {
    const WifiSetupResult r = wifiSetupTick();
    if (r != WS_RUNNING) {
      CardSettings settings;
      settings.artDownload = r == WS_CONNECTED;
      cardSettingsSave(settings);
      Serial.printf("[wifi] setup %s; album art downloads %s\n", r == WS_CONNECTED ? "connected" : "skipped",
                    settings.artDownload ? "on" : "off");
      uiShowMessage("Reading card...", "");
      startLibrary();
    }
  }

  int16_t x, y;
  TouchPhase phase;
  if (touchPoll(x, y, phase)) {
    handleTouch(phase, x, y);
  }

  if (appState == PLAYER) {
    artFetchTick();
    static String artShown;
    const String art = artFetchStatus();
    if (art != artShown && !libraryIndexing()) {
      artShown = art;
      uiSetLibraryStatus(art);
    }
    uiTick();
    if (uiTakeRebuildRequest()) {
      startFullRebuild();
    }
    if (uiTakeWifiSetupRequest()) {
      artFetchStop();
      appState = WIFI_SETUP;
      wifiSetupBegin();
    }
  }
}
