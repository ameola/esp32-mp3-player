#include "Ui.h"

#include "ArtFetch.h"
#include "ArtView.h"
#include "AudioPlayer.h"
#include "CardFile.h"
#include "Catalog.h"
#include "Config.h"
#include "Lcd.h"
#include "LibFormat.h"
#include "LibStore.h"
#include "Text.h"
#include "CardSettings.h"
#include "Keyboard.h"
#include "UiFont.h"
#include "UiTheme.h"
#include "WifiSetup.h"
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include <SD.h>
#include <esp_heap_caps.h>
#include "ThumbStore.h"

// ================================================================ Navigation model
// Each tab has its own stack of levels; switching tabs returns to where you
// were. A level is one list on screen.
enum ListKind : uint8_t {
  L_SONGS,          // all songs by title
  L_ARTISTS,        // all artists
  L_ARTIST_ALBUMS,  // param: artist id. "All songs" row, then the artist's albums
  L_ARTIST_SONGS,   // param: artist id. The artist's songs (album, track order)
  L_ALBUMS,         // all albums by name
  L_ALBUM_SONGS,    // param: album id. Track order
  L_FOLDER,         // param: folder id. Subfolders, then songs
  L_SETTINGS,       // the Settings tab: its groups
  L_SETTINGS_GROUP, // param: SettingGroup. Its settings
  L_SETTING_CHOICE, // param: SettingId. Its choices
  L_SEARCH,         // search results (searchHits)
};

enum RowKind : uint8_t { R_NONE, R_SONG, R_ARTIST, R_ALBUM, R_ALL_SONGS, R_FOLDER, R_SETTING,
                         R_HERO,      // page header (id: 0 top half, 1 bottom half)
                         R_ACTIONS,   // Play all / Shuffle
                         R_GROUP,     // Settings group (id: SettingGroup)
                         R_CHOICE };  // one choice of a setting (id: its index)

struct Level {
  ListKind kind = L_SONGS;
  uint32_t param = 0;
  int32_t top = 0;   // first visible row
  String title;      // for the trail
};

struct Row {
  RowKind kind = R_NONE;
  uint32_t id = 0;
  String line1, line2, right;
  uint32_t art = 0;   // artist, album and song rows: thumbnail key (artArtistKey, artAlbumKey); 0 none
  uint32_t art2 = 0;  // song rows: cover to try if `art` has none (compilations)
  uint16_t icon = 0;  // placeholder / settings icon (0: by kind)
  bool chevron = false;  // settings rows that open something
  bool checked = false;  // the current choice
};

#define MAX_NAV_DEPTH 12

struct Tab {
  const char *label;
  Level stack[MAX_NAV_DEPTH];
  uint8_t depth = 1;
};

static Tab tabs[TAB_COUNT];
static uint8_t activeTab = 0;

static Level &level() {
  Tab &t = tabs[activeTab];
  return t.stack[t.depth - 1];
}

static void resetTabs() {
  static const char *const labels[TAB_COUNT] = {"Songs", "Artists", "Albums", "Folders", "Settings"};
  static const ListKind roots[TAB_COUNT] = {L_SONGS, L_ARTISTS, L_ALBUMS, L_FOLDER, L_SETTINGS};
  for (int t = 0; t < TAB_COUNT; t++) {
    tabs[t].label = labels[t];
    tabs[t].depth = 1;
    tabs[t].stack[0] = Level();
    tabs[t].stack[0].kind = roots[t];
    tabs[t].stack[0].param = 0;  // folder 0 is the card root
    tabs[t].stack[0].title = labels[t];
  }
}

// ---------------------------------------------------------------- Play context
// The list a song was started from; next/previous move through it.
enum CtxKind : uint8_t {
  CTX_NONE,
  CTX_TITLE,       // positions in title.idx
  CTX_ARTIST_IDX,  // positions in artist.idx (an artist's or album's songs)
  CTX_SONG_IDS,    // consecutive song ids (a folder's songs)
  CTX_NONE_IDX,    // positions in none.idx (the Albums tab's "none")
  CTX_SEARCH,      // song hits of the last search
};

struct PlayContext {
  CtxKind kind = CTX_NONE;
  uint32_t start = 0;
  uint32_t count = 0;
};

static PlayContext playCtx;
static int32_t playIndex = -1;  // index of the playing song in playCtx
static Track current;           // the playing song
static String currentPath;

// The last search's results: artists, then albums, then songs.
struct SearchHit {
  uint8_t kind;  // R_ARTIST, R_ALBUM, R_SONG
  uint32_t id;
};
static std::vector<SearchHit> searchHits;
static uint32_t searchSongStart = 0;  // index of the first song hit

static bool ctxSong(const PlayContext &c, uint32_t i, uint32_t &id) {
  if (i >= c.count) {
    return false;
  }
  switch (c.kind) {
    case CTX_TITLE: return Catalog::titleSong(c.start + i, id);
    case CTX_ARTIST_IDX: return Catalog::artistSong(c.start + i, id);
    case CTX_SONG_IDS: id = c.start + i; return true;
    case CTX_NONE_IDX: return Catalog::noneSong(c.start + i, id);
    case CTX_SEARCH:
      if (c.start + i >= searchHits.size()) return false;
      id = searchHits[c.start + i].id;
      return true;
    default: return false;
  }
}

// ---------------------------------------------------------------- Shuffle and repeat
// Shuffle visits every song of the play context once, in a random order that
// starts at the song chosen. The order is a keyed permutation (a 4-round
// Feistel network over the next power of 4, cycle-walked into range), so it
// costs no memory however long the list, and runs backwards for "previous".
enum RepeatMode : uint8_t { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };
static bool shuffleOn = false;
static uint8_t repeatMode = REPEAT_OFF;
static uint32_t shuffleSeed = 1, shuffleOffset = 0, shufflePos = 0;

static uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

static uint32_t feistel(uint32_t v, int half, bool inverse) {
  const uint32_t mask = (1u << half) - 1;
  uint32_t l = v >> half, r = v & mask;
  for (int i = 0; i < 4; i++) {
    const int round = inverse ? 3 - i : i;
    if (!inverse) {
      const uint32_t t = l ^ (mix32(r ^ shuffleSeed ^ round * 0x9e3779b9u) & mask);
      l = r;
      r = t;
    } else {
      const uint32_t t = r ^ (mix32(l ^ shuffleSeed ^ round * 0x9e3779b9u) & mask);
      r = l;
      l = t;
    }
  }
  return l << half | r;
}

// The permutation of 0..n-1 (and its inverse).
static uint32_t shuffleMap(uint32_t i, uint32_t n, bool inverse) {
  int half = 1;
  while ((1u << (2 * half)) < n) half++;
  uint32_t v = i;
  do {
    v = feistel(v, half, inverse);
  } while (v >= n);
  return v;
}

// Shuffled position p -> index in the play context.
static uint32_t shuffledIndex(uint32_t p, uint32_t n) {
  return shuffleMap((p + shuffleOffset) % n, n, false);
}

// A new order that starts at `index`.
static void reshuffle(uint32_t index, uint32_t n) {
  shuffleSeed = mix32(esp_random() | 1);
  shuffleOffset = n ? shuffleMap(index, n, true) : 0;
  shufflePos = 0;
}

static void savePlayModes() {
  Preferences p;
  if (p.begin("play", false)) {
    p.putBool("shuffle", shuffleOn);
    p.putUChar("repeat", repeatMode);
    p.end();
  }
}

static void loadPlayModes() {
  Preferences p;
  if (p.begin("play", true)) {
    shuffleOn = p.getBool("shuffle", false);
    repeatMode = min<uint8_t>(p.getUChar("repeat", REPEAT_OFF), REPEAT_ONE);
    p.end();
  }
}

static bool isPlaying(uint32_t songId) {
  return playIndex >= 0 && current.id == songId;
}

// ---------------------------------------------------------------- Settings
// A small tree on the list machinery: groups, each a list of settings; a
// setting with choices opens a list of them (the current one ticked), an
// action runs on a tap, and an information row shows a value.
enum SettingId : uint8_t {
  S_THEME, S_DENSITY, S_TEXT, S_CORNERS, S_BRIGHTNESS, S_SLEEP,  // Display
  S_WIFI, S_ART, S_ART_NOW,                                       // WiFi & album art
  S_SONGS, S_ARTISTS, S_ALBUMS, S_REBUILD,                        // Library
  S_VERSION, S_BOARD, S_MEMORY, S_CARD, S_UPTIME,                 // System
  SETTING_COUNT
};
enum SettingGroup : uint8_t { G_DISPLAY, G_WIFI, G_LIBRARY, G_SYSTEM, GROUP_COUNT };
static const uint8_t kGroupFirst[GROUP_COUNT + 1] = {S_THEME, S_WIFI, S_SONGS, S_VERSION, SETTING_COUNT};
static const char *const kGroupNames[GROUP_COUNT] = {"Display", "WiFi & album art", "Library", "System"};
static const uint16_t kGroupIcons[GROUP_COUNT] = {ICON_PALETTE, ICON_WIFI, ICON_MUSIC_NOTE, ICON_MEMORY};

static uint32_t rebuildArmedAt = 0;  // first tap on "Rebuild library"; a second within 4 s starts it
static bool rebuildRequested = false;
static bool wifiSetupRequested = false;
static String wifiSsid;              // saved network, "" if none (read when the player screen is shown)

static String formatCount(uint32_t n) {
  String s = String(n);
  for (int i = s.length() - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return s;
}

static uint8_t choiceCount(uint32_t id) {
  switch (id) {
    case S_THEME: return THEME_COUNT;
    case S_DENSITY: case S_TEXT: case S_CORNERS: case S_ART: return 2;
    case S_BRIGHTNESS: return 4;
    case S_SLEEP: return 5;
    default: return 0;
  }
}

static String choiceName(uint32_t id, uint8_t i) {
  switch (id) {
    case S_THEME: return uiThemeName(i);
    case S_DENSITY: return i ? "Compact" : "Comfortable";
    case S_TEXT: return i ? "Large" : "Standard";
    case S_CORNERS: return i ? "Square" : "Round";
    case S_BRIGHTNESS: return uiBrightnessName(i);
    case S_SLEEP: return i == 4 ? String("Never") : String(uiSleepName(i)) + " without a touch";
    case S_ART: return i ? "On, while nothing plays" : "Off";
    default: return "";
  }
}

static uint8_t choiceCurrent(uint32_t id) {
  switch (id) {
    case S_THEME: return uiStyle.theme;
    case S_DENSITY: return uiStyle.compact;
    case S_TEXT: return uiStyle.large;
    case S_CORNERS: return uiStyle.square;
    case S_BRIGHTNESS: return uiStyle.brightness;
    case S_SLEEP: return uiStyle.sleep;
    case S_ART: return artFetchEnabled();
    default: return 0;
  }
}

static const char *settingLabel(uint32_t id) {
  static const char *const labels[SETTING_COUNT] = {
    "Theme", "Density", "Text size", "Corners", "Brightness", "Screen off after",
    "WiFi network", "Album art downloads", "Download now",
    "Songs", "Artists", "Albums", "Rebuild library",
    "Version", "Board", "Free memory", "SD card", "Running for"};
  return id < SETTING_COUNT ? labels[id] : "";
}

static uint16_t settingIcon(uint32_t id) {
  static const uint16_t icons[SETTING_COUNT] = {
    ICON_PALETTE, ICON_DENSITY, ICON_TEXT_SIZE, ICON_CORNER, ICON_BRIGHTNESS, ICON_BEDTIME,
    ICON_WIFI, ICON_IMAGE, ICON_DOWNLOAD,
    ICON_MUSIC_NOTE, ICON_PERSON, ICON_ALBUM, ICON_REFRESH,
    ICON_INFO, ICON_MEMORY, ICON_MEMORY, ICON_FOLDER, ICON_INFO};
  return id < SETTING_COUNT ? icons[id] : ICON_INFO;
}

static String formatTime(uint32_t ms);
static String settingValue(uint32_t id) {
  if (choiceCount(id)) return choiceName(id, choiceCurrent(id));
  switch (id) {
    case S_WIFI: return wifiSsid.length() ? wifiSsid : String("Not set up");
    case S_ART_NOW: return artFetchRunning() ? "Downloading..." : "Find missing covers and photos";
    case S_REBUILD: return rebuildArmedAt ? "Tap again to rebuild" : "Re-reads every song's tags";
    case S_SONGS: return formatCount(Catalog::titleCount());
    case S_ARTISTS: return formatCount(Catalog::artistCount());
    case S_ALBUMS: return formatCount(Catalog::albumIndexCount());
    case S_VERSION: return FIRMWARE_VERSION;
    case S_BOARD:
      return String(ESP.getChipModel()) + ", " + (ESP.getFlashChipSize() >> 20) + " MB" +
             (ESP.getPsramSize() ? ", PSRAM" : "");
    case S_MEMORY: return String(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024) + " KB";
    case S_CARD: return String(SD.cardSize() / (1024.0 * 1024 * 1024), 1) + " GB";
    case S_UPTIME: return formatTime(millis());
    default: return "";
  }
}

static bool isInfoSetting(uint32_t id) {
  return (id >= S_SONGS && id <= S_ALBUMS) || id >= S_VERSION;
}

static String groupSummary(uint8_t g) {
  switch (g) {
    case G_DISPLAY: return String(uiThemeName(uiStyle.theme)) + " \xC2\xB7 " + uiBrightnessName(uiStyle.brightness);
    case G_WIFI: return (wifiSsid.length() ? wifiSsid : String("Not set up")) + " \xC2\xB7 art " + (artFetchEnabled() ? "on" : "off");
    case G_LIBRARY: return formatCount(Catalog::titleCount()) + " songs";
    case G_SYSTEM: return "Version " FIRMWARE_VERSION;
    default: return "";
  }
}

static bool settingsRow(const Level &l, uint32_t i, Row &row) {
  if (l.kind == L_SETTINGS) {
    if (i >= GROUP_COUNT) return false;
    row.kind = R_GROUP;
    row.id = i;
    row.icon = kGroupIcons[i];
    row.line1 = kGroupNames[i];
    row.line2 = groupSummary(i);
    return true;
  }
  if (l.kind == L_SETTINGS_GROUP) {
    const uint32_t id = kGroupFirst[l.param] + i;
    if (id >= kGroupFirst[l.param + 1]) return false;
    row.kind = R_SETTING;
    row.id = id;
    row.icon = settingIcon(id);
    row.line1 = settingLabel(id);
    if (isInfoSetting(id)) {
      row.right = settingValue(id);
    } else {
      row.line2 = settingValue(id);
      row.chevron = choiceCount(id) || id == S_WIFI;
    }
    return true;
  }
  if (i >= choiceCount(l.param)) return false;  // L_SETTING_CHOICE
  row.kind = R_CHOICE;
  row.id = i;
  row.line1 = choiceName(l.param, i);
  row.checked = i == choiceCurrent(l.param);
  return true;
}

static uint32_t settingsCount(const Level &l) {
  if (l.kind == L_SETTINGS) return GROUP_COUNT;
  if (l.kind == L_SETTINGS_GROUP) return kGroupFirst[l.param + 1] - kGroupFirst[l.param];
  return choiceCount(l.param);
}

// ---------------------------------------------------------------- Level contents
static PlayContext levelContext(const Level &l) {
  PlayContext c;
  ArtistRecord a;
  AlbumRecord al;
  FolderRecord f;
  switch (l.kind) {
    case L_SONGS:
      c = {CTX_TITLE, 0, Catalog::titleCount()};
      break;
    case L_ARTIST_SONGS:
      if (Catalog::artist(l.param, a)) c = {CTX_ARTIST_IDX, a.firstSongPos, a.songCount};
      break;
    case L_ALBUM_SONGS:
      if (Catalog::album(l.param, al)) {
        c = {al.flags & ALBUM_NONE_ALL ? CTX_NONE_IDX : CTX_ARTIST_IDX, al.firstSongPos, al.songCount};
      }
      break;
    case L_FOLDER:
      if (Catalog::folder(l.param, f)) c = {CTX_SONG_IDS, f.firstSong, f.songCount};
      break;
    case L_SEARCH:
      c = {CTX_SEARCH, searchSongStart, static_cast<uint32_t>(searchHits.size()) - searchSongStart};
      break;
    default:
      break;
  }
  return c;
}

// Rows before the first song row (a folder lists its subfolders first).
// Artist and album pages start with a header: two rows of photo and names,
// then a row of Play all / Shuffle buttons.
#define HERO_ROWS 3
static bool hasHero(const Level &l) {
  return l.kind == L_ARTIST_ALBUMS || l.kind == L_ALBUM_SONGS;
}

static uint32_t songRowOffset(const Level &l) {
  FolderRecord f;
  if (l.kind == L_ALBUM_SONGS) return HERO_ROWS;
  if (l.kind == L_SEARCH) return searchSongStart;
  return (l.kind == L_FOLDER && Catalog::folder(l.param, f)) ? f.childCount : 0;
}

static int32_t levelCount(const Level &l) {
  ArtistRecord a;
  AlbumRecord al;
  FolderRecord f;
  switch (l.kind) {
    case L_SONGS: return Catalog::titleCount();
    case L_ARTISTS: return Catalog::artistCount();
    case L_ARTIST_ALBUMS: return Catalog::artist(l.param, a) ? HERO_ROWS + a.albumCount + 1 : 0;
    case L_ARTIST_SONGS: return Catalog::artist(l.param, a) ? a.songCount : 0;
    case L_ALBUMS: return Catalog::albumIndexCount();
    case L_ALBUM_SONGS: return Catalog::album(l.param, al) ? HERO_ROWS + al.songCount : 0;
    case L_FOLDER: return Catalog::folder(l.param, f) ? f.childCount + f.songCount : 0;
    case L_SETTINGS:
    case L_SETTINGS_GROUP:
    case L_SETTING_CHOICE: return settingsCount(l);
    case L_SEARCH: return searchHits.size();
  }
  return 0;
}

static const JumpTable *levelJump(const Level &l) {
  switch (l.kind) {
    case L_SONGS: return Catalog::jump(JUMP_TITLES);
    case L_ARTISTS: return Catalog::jump(JUMP_ARTISTS);
    case L_ALBUMS: return Catalog::jump(JUMP_ALBUMS);
    default: return nullptr;
  }
}

static String plural(uint32_t n, const char *word) {
  return String(n) + " " + word + (n == 1 ? "" : "s");
}

static String formatTime(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char text[12];
  if (s >= 3600) {
    snprintf(text, sizeof(text), "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  } else {
    snprintf(text, sizeof(text), "%lu:%02lu", s / 60, s % 60);
  }
  return text;
}

static bool songRow(uint32_t id, bool showTrackNumbers, Row &row) {
  Track t;
  if (!Catalog::song(id, t)) {
    return false;
  }
  row.kind = R_SONG;
  row.id = id;
  row.line1 = t.title;
  row.line2 = t.artist.isEmpty() ? String("Unknown artist") : t.artist;
  if (showTrackNumbers && t.track > 0) {
    row.line2 = String(t.track) + ". " + row.line2;
  }
  row.right = t.durationMs ? formatTime(t.durationMs) : String("");
  // The album's cover key as the indexer names the album: album artist, else
  // track artist, and the album name cut to AlbumRecord's size. A compilation
  // (no album artist, several track artists) is filed under "Various
  // Artists", which a song can't tell on its own, so that's the fallback.
  if (t.album.length()) {
    const String album = t.album.substring(0, sizeof(AlbumRecord::name) - 1);
    const String &artist = t.albumArtist.length() ? t.albumArtist : t.artist;
    row.art = artAlbumKey(artist.length() ? artist.c_str() : "Unknown Artist", album.c_str());
    if (t.albumArtist.isEmpty()) row.art2 = artAlbumKey("Various Artists", album.c_str());
  }
  return true;
}

static bool levelRow(const Level &l, uint32_t i, Row &row) {
  ArtistRecord a;
  AlbumRecord al;
  FolderRecord f, child;
  uint32_t id;
  switch (l.kind) {
    case L_ARTISTS:
      if (!Catalog::artist(i, a)) return false;
      row.kind = R_ARTIST;
      row.id = i;
      row.line1 = a.name;
      row.line2 = plural(a.albumCount, "album") + ", " + plural(a.songCount, "song");
      row.art = artArtistKey(a.name);
      return true;
    case L_ARTIST_ALBUMS:
      if (!Catalog::artist(l.param, a)) return false;
      if (i < HERO_ROWS) {
        row.kind = i < 2 ? R_HERO : R_ACTIONS;
        row.id = i;
        return true;
      }
      i -= HERO_ROWS;
      if (i == a.albumCount) {
        row.kind = R_ALL_SONGS;
        row.id = l.param;
        row.line1 = "All songs";
        row.line2 = plural(a.songCount, "song");
        row.icon = ICON_MUSIC_NOTE;
        return true;
      }
      if (!Catalog::album(a.firstAlbum + i, al)) return false;
      row.kind = R_ALBUM;
      row.id = a.firstAlbum + i;
      row.line1 = al.name;
      row.line2 = (al.year ? String(al.year) + ", " : String("")) + plural(al.songCount, "song");
      if (al.flags & ALBUM_NONE) row.icon = ICON_STACK;
      else row.art = artAlbumKey(a.name, al.name);
      return true;
    case L_ALBUMS:
      if (!Catalog::albumByName(i, id) || !Catalog::album(id, al)) return false;
      row.kind = R_ALBUM;
      row.id = id;
      row.line1 = al.name;
      if (al.flags & ALBUM_NONE_ALL) {  // every artist's loose songs
        row.icon = ICON_STACK;
        row.line2 = plural(al.songCount, "song") + ", every artist";
        return true;
      }
      row.line2 = Catalog::artist(al.artistId, a) ? String(a.name) : String("");
      row.art = artAlbumKey(row.line2.c_str(), al.name);
      if (al.year) row.line2 += ", " + String(al.year);
      return true;
    case L_FOLDER:
      if (!Catalog::folder(l.param, f)) return false;
      if (i < f.childCount) {
        if (!Catalog::folder(f.firstChild + i, child)) return false;
        row.kind = R_FOLDER;
        row.id = f.firstChild + i;
        row.line1 = child.name;
        row.line2 = child.childCount || child.songCount
                      ? (child.childCount ? plural(child.childCount, "folder") + ", " : String("")) +
                          plural(child.songCount, "song")
                      : String("Empty");
        return true;
      }
      return songRow(f.firstSong + (i - f.childCount), false, row);
    case L_SETTINGS:
    case L_SETTINGS_GROUP:
    case L_SETTING_CHOICE:
      return settingsRow(l, i, row);
    case L_SEARCH: {
      if (i >= searchHits.size()) return false;
      const SearchHit &h = searchHits[i];
      if (h.kind == R_SONG) return songRow(h.id, false, row);
      if (h.kind == R_ARTIST) {
        Level artists;
        artists.kind = L_ARTISTS;
        return levelRow(artists, h.id, row);
      }
      if (!Catalog::album(h.id, al)) return false;  // R_ALBUM
      row.kind = R_ALBUM;
      row.id = h.id;
      row.line1 = al.name;
      row.line2 = Catalog::artist(al.artistId, a) ? String(a.name) : String("");
      row.art = artAlbumKey(row.line2.c_str(), al.name);
      if (al.year) row.line2 += ", " + String(al.year);
      return true;
    }
    default: {  // song lists
      uint32_t songId;
      // Track numbers in an album, not in a "none" album (by title).
      if (hasHero(l) && i < HERO_ROWS) {
        row.kind = i < 2 ? R_HERO : R_ACTIONS;
        row.id = i;
        return true;
      }
      const bool tracks = l.kind == L_ALBUM_SONGS && Catalog::album(l.param, al) &&
                          !(al.flags & (ALBUM_NONE | ALBUM_NONE_ALL));
      return ctxSong(levelContext(l), i - songRowOffset(l), songId) && songRow(songId, tracks, row);
    }
  }
}

// ================================================================ Scrolling text
// A one-line text strip that scrolls horizontally when the text is wider than
// the strip: pause, scroll through to the start of a repeated copy, repeat.
struct Marquee {
  bool active = false;  // true while the text is too wide and on screen
  String text;
  int16_t x = 0, y = 0, w = 0, h = 0;  // strip position on screen
  const UiFont *font = &FONT_BODY;
  uint16_t fg = 0, bg = 0;
  int16_t textW = 0;
  int16_t offset = 0;
  uint32_t nextStep = 0;
};

#define MARQUEE_GAP 40       // pixels between the end of the text and its repeat
#define MARQUEE_STEP_PX 2
#define MARQUEE_STEP_MS 60
#define MARQUEE_PAUSE_MS 2000

static Marquee listMarquee;   // playing row's title in the list
static Marquee titleMarquee;  // Now Playing title

// y: top of the strip on screen (its height is the font's line).
static void marqueeSet(Marquee &m, const String &text, int16_t x, int16_t y, int16_t w, const UiFont &font,
                       uint16_t fg, uint16_t bg) {
  m.text = text;
  m.x = x;
  m.y = y;
  m.w = w;
  m.font = &font;
  m.h = font.ascent + font.descent;
  m.fg = fg;
  m.bg = bg;
  m.textW = uiTextWidth(font, text.c_str());
  m.offset = 0;
  m.nextStep = millis() + MARQUEE_PAUSE_MS;
  m.active = m.textW > w;
}

// Draws the strip into the canvas with its top-left at (ox, oy), clipped to
// the strip.
static void marqueeCompose(const Marquee &m, int16_t ox, int16_t oy) {
  canvas->fillRect(ox, oy, m.w, m.h, m.bg);
  const int16_t base = oy + m.font->ascent;
  uiDrawText(*m.font, m.text.c_str(), ox - m.offset, base, m.fg, ox, ox + m.w);
  if (m.offset > 0) uiDrawText(*m.font, m.text.c_str(), ox - m.offset + m.textW + MARQUEE_GAP, base, m.fg, ox, ox + m.w);
}

static void listBlitMarquee(const Marquee &m);

static void marqueeTick(Marquee &m) {
  if (!m.active || static_cast<int32_t>(millis() - m.nextStep) < 0) {
    return;
  }
  m.offset += MARQUEE_STEP_PX;
  if (m.offset >= m.textW + MARQUEE_GAP) {
    m.offset = 0;
    m.nextStep = millis() + MARQUEE_PAUSE_MS;
  } else {
    m.nextStep = millis() + MARQUEE_STEP_MS;
  }
  marqueeCompose(m, 0, 0);
  if (m.y >= LIST_Y && m.y < LIST_END) {
    if (m.y + m.h <= LIST_END) listBlitMarquee(m);
  } else {
    lcdBlit(m.x, m.y, m.w, m.h);
  }
}

// ================================================================ List window
// The list area scrolls in hardware: screen line LIST_Y+i shows frame memory
// line LIST_Y + (i + listOffset) % LIST_H. Everything drawn into the list
// area (rows, letter strip, the playing row's marquee) goes through these.
#define LIST_H (LIST_END - LIST_Y)
static int16_t listOffset = 0;
static bool scrollAreaSet = false;

static void setListOffset(int16_t offset) {
  offset = ((offset % LIST_H) + LIST_H) % LIST_H;
  if (offset != listOffset) {
    listOffset = offset;
    lcdScrollOffset(offset);
  }
}

// Blits canvas lines fromY.. (h lines) to the list area at screen line y.
static void listBlit(int16_t x, int16_t y, int16_t w, int16_t h, int16_t fromY = 0) {
  const int16_t mem = (y - LIST_Y + listOffset) % LIST_H;
  const int16_t first = min<int16_t>(h, LIST_H - mem);
  lcdBlitFrom(fromY, x, LIST_Y + mem, w, first);
  if (first < h) {
    lcdBlitFrom(fromY + first, x, LIST_Y, w, h - first);
  }
}

static void listFill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  const int16_t mem = (y - LIST_Y + listOffset) % LIST_H;
  const int16_t first = min<int16_t>(h, LIST_H - mem);
  lcdFillRect(x, LIST_Y + mem, w, first, color);
  if (first < h) {
    lcdFillRect(x, LIST_Y, w, h - first, color);
  }
}

static void listBlitMarquee(const Marquee &m) {
  listBlit(m.x, m.y, m.w, m.h);
}

// Full-screen layouts other than the player expect an unscrolled screen.
static void resetListScroll() {
  setListOffset(0);
}

static void dropNow();

// ================================================================ Drawing
// Each draw function composes its region at (0,0) of the canvas, then blits.
static String libraryStatus;
static int lastProgressPx = -1;
static int32_t lastTimeShown = -1;  // elapsed seconds currently on screen
static uint8_t wifiShown = 0xFF;    // WiFi state in the header (see wifiState())
static int16_t searchIconX = -1;  // the header's search icon, -1 when not shown

// Centred text with its top near y: size 2 is a title, size 1 small text.
static void drawCenteredText(const String &text, int16_t cx, int16_t y, uint8_t size, uint16_t color) {
  const UiFont &f = size >= 2 ? FONT_TITLE : FONT_SMALL;
  const String fitted = uiFitText(f, text, SCREEN_W - 16);
  uiDrawText(f, fitted.c_str(), cx - uiTextWidth(f, fitted.c_str()) / 2, y + (size >= 2 ? 13 : 9), color);
}

static void drawCentered(const UiFont &f, const String &text, int16_t cx, int16_t baseline, uint16_t color) {
  uiDrawText(f, text.c_str(), cx - uiTextWidth(f, text.c_str()) / 2, baseline, color);
}

// Anti-aliased rounded corners on a square already in the canvas: blends
// the corners toward bg (r = size / 2 makes a circle).
static void roundCorners(int16_t x, int16_t y, int16_t size, int16_t r, uint16_t bg) {
  if (r <= 0) return;
  uint16_t *buf = canvas->getBuffer();
  const int16_t cw = canvas->width(), ch = canvas->height();
  auto blend = [&](int16_t px, int16_t py, uint8_t a) {
    if (px >= 0 && px < cw && py >= 0 && py < ch) buf[py * cw + px] = uiBlend(buf[py * cw + px], bg, a);
  };
  for (int16_t j = 0; j < r; j++) {
    for (int16_t i = 0; i < r; i++) {
      const float dx = r - i - 0.5f, dy = r - j - 0.5f;
      const float inside = constrain(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
      if (inside >= 1) continue;
      const uint8_t a = lroundf((1 - inside) * 15);
      blend(x + i, y + j, a);
      blend(x + size - 1 - i, y + j, a);
      blend(x + i, y + size - 1 - j, a);
      blend(x + size - 1 - i, y + size - 1 - j, a);
    }
  }
}

// A filled anti-aliased circle centred at (cx, cy).
static void drawDot(int16_t cx, int16_t cy, int16_t r, uint16_t color, uint16_t bg) {
  canvas->fillRect(cx - r, cy - r, 2 * r, 2 * r, color);
  roundCorners(cx - r, cy - r, 2 * r, r, bg);
}

// WiFi in the header: 0 not set up or downloads off, 1 idle (connects to
// download), 2 connected (a download pass is running).
static uint8_t wifiState() {
  if (artFetchRunning()) return 2;
  return wifiSsid.length() && artFetchEnabled() ? 1 : 0;
}

// Status at the right end of the header: library status text, WiFi icon.
// Returns its left edge.
static int16_t composeStatus() {
  int16_t x = SCREEN_W - 8;
  const uint8_t wifi = wifiState();
  wifiShown = wifi;
  uiDrawIcon(ICONS_14, wifi ? ICON_WIFI : ICON_WIFI_OFF, x - 14, 9, wifi == 2 ? pal.accent : pal.faint);
  x -= 20;
  if (libraryStatus.length()) {
    String text = libraryStatus;
    text.replace("\n", " ");
    const int16_t w = uiTextWidth(FONT_LABEL, text.c_str());
    uiDrawText(FONT_LABEL, text.c_str(), x - w, 20, pal.accent);
    x -= w + 6;
  }
  return x;
}

// Back button (below a tab's top) or the tab's name with its count, then
// the status.
static void drawHeader() {
  canvas->fillRect(0, 0, SCREEN_W, HEADER_H, pal.bg);
  const Tab &tab = tabs[activeTab];
  const Level &l = level();
  const int16_t statusX = composeStatus();
  int16_t x = 10;
  if (tab.depth > 1) {
    uiDrawIcon(ICONS_24, ICON_BACK, 4, 4, pal.text);
    x = 32;
  }
  const bool counted = l.kind == L_SONGS || l.kind == L_ARTISTS || l.kind == L_ALBUMS;
  const String count = tab.depth == 1 && counted ? formatCount(levelCount(l)) : String();
  int16_t room = statusX - 6 - x;
  searchIconX = -1;
  if (tab.depth == 1 && counted) {  // search
    searchIconX = statusX - 22;
    uiDrawIcon(ICONS_18, ICON_SEARCH, searchIconX, 7, pal.text);
    room -= 26;
  }
  if (count.length()) room -= uiTextWidth(FONT_SMALL, count.c_str()) + 5;
  if (tab.depth > 1 && hasHero(l)) {  // the page's own name is in its header: show where it came from
    const String &parent = tab.stack[tab.depth - 2].title;
    uiDrawText(FONT_BODY, uiFitText(FONT_BODY, parent, room).c_str(), x, 21, pal.dim);
    lcdBlit(0, 0, SCREEN_W, HEADER_H);
    return;
  }
  const int16_t after = uiDrawText(FONT_TITLE, uiFitText(FONT_TITLE, l.title, room).c_str(), x, 22, pal.text);
  if (count.length()) uiDrawText(FONT_SMALL, count.c_str(), after + 5, 22, pal.dim);
  lcdBlit(0, 0, SCREEN_W, HEADER_H);
}

static const uint16_t kTabIcons[TAB_COUNT] = {ICON_MUSIC_NOTE, ICON_PERSON, ICON_ALBUM, ICON_FOLDER, ICON_TUNE};

static void drawTabs() {
  canvas->fillRect(0, 0, SCREEN_W, TABBAR_H, pal.bg);
  canvas->drawFastHLine(0, 0, SCREEN_W, pal.line);
  for (int t = 0; t < TAB_COUNT; t++) {
    const uint16_t color = t == activeTab ? pal.accent : pal.faint;
    uiDrawIcon(ICONS_18, kTabIcons[t], t * TAB_W + (TAB_W - 18) / 2, 4, color);
    drawCentered(FONT_TAB, tabs[t].label, t * TAB_W + TAB_W / 2, 33, color);
  }
  lcdBlit(0, TABBAR_Y, SCREEN_W, TABBAR_H);
}

// ================================================================ Thumbnails
// Artist, album and song rows show a 30 px thumbnail from the ThumbStore
// (one open file: a seek and a 2 KB read, a few ms). The last few are kept
// in RAM so moving the list doesn't read the card for every frame, and keys
// with none are remembered until the next tab switch. The download pass
// makes the thumbnails; the list only reads them.
#define THUMB_SLOTS 8
struct ThumbSlot {
  uint32_t key = 0;  // 0 = free
  uint32_t used = 0;
};
static ThumbSlot thumbSlots[THUMB_SLOTS];
static uint16_t *thumbPixels = nullptr;  // THUMB_SLOTS x ART_THUMB_BYTES, while needed
static uint32_t thumbClock = 0;
static bool thumbPending = false;  // a row shows a placeholder it may not need
static ThumbStore thumbStore;
// Store reads left for this frame of a moving list; rows past it show the
// placeholder until the list stops.
#define THUMB_LOOKUPS_PER_FRAME 2
static int thumbLookups = THUMB_LOOKUPS_PER_FRAME;
// Keys known to have no thumbnail, so their rows don't look again.
#define THUMB_MISSING 64
static uint32_t thumbMissing[THUMB_MISSING];
static uint8_t thumbMissingNext = 0;

static void releaseThumbs() {
  free(thumbPixels);
  thumbPixels = nullptr;
  for (ThumbSlot &t : thumbSlots) t = ThumbSlot();
  memset(thumbMissing, 0, sizeof(thumbMissing));  // pictures may have arrived meanwhile
  thumbStore.retry();                             // and the store been created
}

// The thumbnail for a key, nullptr for none (or not yet, see thumbPending).
// *missing is set when it's known not to exist.
static const uint16_t *thumbFor(uint32_t key, bool atRest, bool *missing) {
  *missing = false;
  if (!thumbPixels) {
    thumbPixels = static_cast<uint16_t *>(malloc(THUMB_SLOTS * ART_THUMB_BYTES));
    if (!thumbPixels) return nullptr;
  }
  int slot = -1;
  for (int i = 0; i < THUMB_SLOTS; i++) {
    if (thumbSlots[i].key == key) slot = i;
  }
  if (slot < 0) {
    for (uint32_t m : thumbMissing) {
      if (m == key) {
        *missing = true;
        return nullptr;
      }
    }
    if (!atRest && thumbLookups <= 0) {
      thumbPending = true;
      return nullptr;
    }
    thumbLookups--;
    slot = 0;  // least recently used
    for (int i = 1; i < THUMB_SLOTS; i++) {
      if (thumbSlots[i].used < thumbSlots[slot].used) slot = i;
    }
    if (!thumbStore.read(key, thumbPixels + slot * (ART_THUMB * ART_THUMB))) {
      thumbSlots[slot] = ThumbSlot();
      thumbMissing[thumbMissingNext] = key;
      thumbMissingNext = (thumbMissingNext + 1) % THUMB_MISSING;
      *missing = true;
      return nullptr;
    }
    thumbSlots[slot].key = key;
  }
  thumbSlots[slot].used = ++thumbClock;
  return thumbPixels + slot * (ART_THUMB * ART_THUMB);
}

// ---------------------------------------------------------------- Page header
// Artist and album pages: a 60 px photo or cover, the name and a details
// line over two rows, then Play all / Shuffle. The picture is decoded once
// per page (artist photo or album cover JPEG at 1/4; else the 30 px
// thumbnail enlarged) into a 7 KB buffer, freed when the page is left.
#define HERO_PHOTO 60
struct HeroInfo {
  ListKind kind = L_SONGS;
  uint32_t param = LIB_NONE;
  String name, sub;
  bool round = false;
  uint16_t icon = ICON_MUSIC_NOTE;
  uint16_t *photo = nullptr;
};
static HeroInfo hero;

static void heroFree() {
  free(hero.photo);
  hero = HeroInfo();
}

// 30 px thumbnail to 60 px, bilinear.
static void enlargeThumb(const uint16_t *src, uint16_t *dst) {
  for (int y = 0; y < HERO_PHOTO; y++) {
    const float fy = max(0.0f, (y + 0.5f) / 2 - 0.5f);
    const int y0 = min<int>(fy, ART_THUMB - 1), y1 = min<int>(y0 + 1, ART_THUMB - 1);
    const uint8_t ty = (fy - y0) * 15;
    for (int x = 0; x < HERO_PHOTO; x++) {
      const float fx = max(0.0f, (x + 0.5f) / 2 - 0.5f);
      const int x0 = min<int>(fx, ART_THUMB - 1), x1 = min<int>(x0 + 1, ART_THUMB - 1);
      const uint8_t tx = (fx - x0) * 15;
      const uint16_t top = uiBlend(src[y0 * ART_THUMB + x0], src[y0 * ART_THUMB + x1], tx);
      const uint16_t bottom = uiBlend(src[y1 * ART_THUMB + x0], src[y1 * ART_THUMB + x1], tx);
      dst[y * HERO_PHOTO + x] = uiBlend(top, bottom, ty);
    }
  }
}

static void loadHero(const Level &l) {
  if (hero.kind == l.kind && hero.param == l.param) return;
  heroFree();
  hero.kind = l.kind;
  hero.param = l.param;
  ArtistRecord a;
  AlbumRecord al;
  String jpg;
  uint32_t key = 0;
  if (l.kind == L_ARTIST_ALBUMS && Catalog::artist(l.param, a)) {
    hero.name = a.name;
    hero.sub = plural(a.albumCount, "album") + " \xC2\xB7 " + plural(a.songCount, "song");
    hero.round = true;
    hero.icon = ICON_PERSON;
    jpg = artArtistPhotoPath(a.name);
    key = artArtistKey(a.name);
  } else if (l.kind == L_ALBUM_SONGS && Catalog::album(l.param, al)) {
    hero.name = al.name;
    if (al.flags & ALBUM_NONE_ALL) {
      hero.sub = formatCount(al.songCount) + " songs \xC2\xB7 every artist, A\xE2\x80\x93Z";
      hero.icon = ICON_STACK;
    } else if (Catalog::artist(al.artistId, a)) {
      if (al.flags & ALBUM_NONE) {
        hero.sub = String(a.name) + " \xC2\xB7 " + plural(al.songCount, "song") + ", A\xE2\x80\x93Z";
        hero.icon = ICON_STACK;
      } else {
        hero.sub = String(a.name) + (al.year ? " \xC2\xB7 " + String(al.year) : String("")) + " \xC2\xB7 " +
                   plural(al.songCount, "song");
        hero.icon = ICON_ALBUM;
        jpg = artCoverPath(a.name, al.name);
        key = artAlbumKey(a.name, al.name);
      }
    }
  }
  FILINFO fi;
  if (jpg.length() && f_stat(cardPath(jpg).c_str(), &fi) == FR_OK) {
    hero.photo = static_cast<uint16_t *>(malloc(HERO_PHOTO * HERO_PHOTO * 2));
    if (hero.photo && !artDecodeJpeg(cardPath(jpg), hero.photo, HERO_PHOTO, pal.surface2)) {
      free(hero.photo);
      hero.photo = nullptr;
    }
  }
  if (!hero.photo && key) {
    bool missing;
    const uint16_t *px = thumbFor(key, true, &missing);
    if (px && (hero.photo = static_cast<uint16_t *>(malloc(HERO_PHOTO * HERO_PHOTO * 2)))) enlargeThumb(px, hero.photo);
  }
}

// Rounded corners on a rectangle (r <= half its smaller side).
static void roundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t bg) {
  if (r <= 0) return;
  uint16_t *buf = canvas->getBuffer();
  const int16_t cw = canvas->width(), ch = canvas->height();
  auto blend = [&](int16_t px, int16_t py, uint8_t a) {
    if (px >= 0 && px < cw && py >= 0 && py < ch) buf[py * cw + px] = uiBlend(buf[py * cw + px], bg, a);
  };
  for (int16_t j = 0; j < r; j++) {
    for (int16_t i = 0; i < r; i++) {
      const float dx = r - i - 0.5f, dy = r - j - 0.5f;
      const float inside = constrain(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
      if (inside >= 1) continue;
      const uint8_t a = lroundf((1 - inside) * 15);
      blend(x + i, y + j, a);
      blend(x + w - 1 - i, y + j, a);
      blend(x + i, y + h - 1 - j, a);
      blend(x + w - 1 - i, y + h - 1 - j, a);
    }
  }
}

// Half `part` (0 top, 1 bottom) of the two-row header.
static void composeHero(uint8_t part, uint16_t bg) {
  const int16_t h = 2 * ROW_H, oy = -part * ROW_H;
  const int16_t px = 10, py = oy + (h - HERO_PHOTO) / 2;
  if (hero.photo) {
    uint16_t *buf = canvas->getBuffer();
    const int16_t cw = canvas->width(), ch = canvas->height();
    for (int j = 0; j < HERO_PHOTO; j++) {
      const int y = py + j;
      if (y >= 0 && y < ch) memcpy(buf + y * cw + px, hero.photo + j * HERO_PHOTO, HERO_PHOTO * 2);
    }
  } else {
    canvas->fillRect(px, py, HERO_PHOTO, HERO_PHOTO, pal.surface2);
    uiDrawIcon(ICONS_24, hero.icon, px + 18, py + 18, hero.icon == ICON_STACK ? pal.accent : pal.faint);
  }
  roundCorners(px, py, HERO_PHOTO, hero.round ? (uiStyle.square ? 0 : HERO_PHOTO / 2) : uiRadiusCard(), bg);
  const int16_t tx = px + HERO_PHOTO + 12, tw = LIST_W - tx - 8;
  uiDrawText(FONT_HEADLINE, uiFitText(FONT_HEADLINE, hero.name, tw).c_str(), tx, oy + h / 2 + 2, pal.text);
  uiDrawText(FONT_SMALL, uiFitText(FONT_SMALL, hero.sub, tw).c_str(), tx, oy + h / 2 + 18, pal.dim);
}

// Play all | Shuffle.
#define ACTIONS_GAP 10
static void composeActions(const Level &l) {
  const int16_t h = 28, y = (ROW_H - h) / 2, w = (LIST_W - 2 * 10 - ACTIONS_GAP) / 2;
  const struct {
    uint16_t icon;
    const char *label;
    uint16_t fill, ink;
  } buttons[2] = {{ICON_PLAY, l.kind == L_ARTIST_ALBUMS ? "Play all" : "Play", pal.accent, pal.onAccent},
                  {ICON_SHUFFLE, "Shuffle", pal.surface2, pal.text}};
  for (int b = 0; b < 2; b++) {
    const int16_t x = 10 + b * (w + ACTIONS_GAP);
    canvas->fillRect(x, y, w, h, buttons[b].fill);
    roundRect(x, y, w, h, uiRadiusPill(h), pal.bg);
    const int16_t tw = uiTextWidth(FONT_LABEL, buttons[b].label), cx = x + (w - 18 - 4 - tw) / 2;
    uiDrawIcon(ICONS_18, buttons[b].icon, cx, y + 5, buttons[b].ink);
    uiDrawText(FONT_LABEL, buttons[b].label, cx + 22, y + 18, buttons[b].ink);
  }
}

// The thumbnail slot: a cover, a photo or a placeholder icon, 30 px at
// (x, y), corners rounded per the Corners setting (artists: round).
static void composeArt(const Row *row, int16_t x, int16_t y, bool atRest, uint16_t bg) {
  const bool person = row->kind == R_ARTIST || row->kind == R_ALL_SONGS;
  const int16_t radius = person ? (uiStyle.square ? 0 : ART_THUMB / 2) : uiRadiusArt();
  const uint16_t *px = nullptr;
  bool missing = true;
  if (row->art) px = thumbFor(row->art, atRest, &missing);
  if (!px && missing && row->art2) px = thumbFor(row->art2, atRest, &missing);
  if (px) {
    canvas->drawRGBBitmap(x, y, px, ART_THUMB, ART_THUMB);
  } else {
    uint16_t icon = row->icon;
    if (!icon) {
      icon = person ? ICON_PERSON : row->kind == R_ALBUM ? ICON_ALBUM : row->kind == R_FOLDER ? ICON_FOLDER : ICON_MUSIC_NOTE;
    }
    const bool accentIcon = row->kind == R_SETTING || row->kind == R_GROUP || icon == ICON_STACK;
    canvas->fillRect(x, y, ART_THUMB, ART_THUMB, pal.surface2);
    uiDrawIcon(ICONS_18, icon, x + (ART_THUMB - 18) / 2, y + (ART_THUMB - 18) / 2, accentIcon ? pal.accent : pal.faint);
  }
  roundCorners(x, y, ART_THUMB, row->kind == R_SETTING || row->kind == R_GROUP ? (uiStyle.square ? 0 : 6) : radius, bg);
}

// Composes list row `index` (data `row`, or nullptr for an empty row) into the
// canvas at (0,0). When rowTop >= 0 the row is at rest with its top at that
// screen line and a playing row's title scrolls; while the list moves it's
// drawn trimmed instead.
static void composeRow(int32_t index, const Row *row, int16_t rowTop) {
  const int16_t h = ROW_H;
  const bool playing = row && row->kind == R_SONG && isPlaying(row->id);
  const uint16_t bg = playing ? pal.playing : pal.bg;
  canvas->fillRect(0, 0, LIST_W, h, bg);
  if (!row) {
    return;
  }
  if (row->kind == R_HERO) {
    loadHero(level());
    composeHero(row->id, bg);
    return;
  }
  if (row->kind == R_ACTIONS) {
    composeActions(level());
    return;
  }
  const UiFont &body = uiFontBody(), &small = uiFontSmall();
  const bool drill = row->kind == R_SETTING ? row->chevron : row->kind != R_SONG && row->kind != R_CHOICE;
  const int16_t slotX = 8;
  int16_t textX = slotX + ART_THUMB + 9;
  int16_t right = LIST_W - 6;
  const int16_t slotY = (h - ART_THUMB) / 2;
  if (row->kind == R_CHOICE) {
    if (level().param == S_THEME) {  // a swatch of the theme
      const UiPalette &p = uiThemePalette(row->id);
      canvas->fillRect(slotX, slotY, ART_THUMB, ART_THUMB, p.bg);
      canvas->fillRect(slotX + 5, slotY + 18, 20, 3, p.text);
      canvas->fillRect(slotX + 5, slotY + 23, 13, 2, p.dim);
      drawDot(slotX + 20, slotY + 10, 5, p.accent, p.bg);
      canvas->drawRect(slotX, slotY, ART_THUMB, ART_THUMB, pal.line);
      roundCorners(slotX, slotY, ART_THUMB, uiStyle.square ? 0 : 6, bg);
    } else {
      textX = 14;
    }
    if (row->checked) {
      uiDrawIcon(ICONS_18, ICON_CHECK, LIST_W - 26, (h - 18) / 2, pal.accent);
      right = LIST_W - 30;
    }
  } else {
    composeArt(row, slotX, slotY, rowTop >= 0, bg);
  }
  if (drill) {
    uiDrawIcon(ICONS_14, ICON_CHEVRON, LIST_W - 18, (h - 14) / 2, pal.faint);
    right = LIST_W - 22;
  }
  const bool twoLines = row->line2.length() > 0;
  const int16_t base1 = twoLines ? h / 2 - 3 : h / 2 + body.ascent / 3;
  const int16_t base2 = base1 + small.ascent + 2;
  if (row->right.length()) {
    const int16_t w = uiTextWidth(small, row->right.c_str());
    uiDrawText(small, row->right.c_str(), right - w, h / 2 + small.ascent / 3, playing ? pal.accent : pal.dim);
    right -= w + 8;
  }
  const int16_t textW = right - textX;
  const bool wholeRow = rowTop >= 0 && rowTop + h <= LIST_END;
  if (playing && wholeRow) {
    marqueeSet(listMarquee, row->line1, textX, rowTop + base1 - body.ascent, textW, body, pal.accent, bg);
    marqueeCompose(listMarquee, textX, base1 - body.ascent);
  } else {
    uiDrawText(body, uiFitText(body, row->line1, textW).c_str(), textX, base1, playing ? pal.accent : pal.text);
  }
  if (twoLines) {
    uiDrawText(small, uiFitText(small, row->line2, textW).c_str(), textX, base2, pal.dim);
  }
}

static void drawRow(int r) {
  const Level &l = level();
  const int32_t index = l.top + r;
  const int16_t rowY = LIST_Y + r * ROW_H;
  if (rowY >= LIST_END) {
    return;
  }
  const int32_t count = levelCount(l);
  Row row;
  const bool have = index < count && levelRow(l, index, row);
  const bool playing = have && row.kind == R_SONG && isPlaying(row.id);
  if (!playing && listMarquee.y >= rowY && listMarquee.y < rowY + ROW_H) {
    listMarquee.active = false;  // this row no longer shows the playing song
  }
  composeRow(index, have ? &row : nullptr, rowY);
  if (!have && r == 0 && count == 0) {
    uiDrawText(FONT_SMALL, Catalog::songCount() ? "Nothing here" : "No MP3 files found", 10, 24, pal.dim);
  }
  listBlit(0, rowY, LIST_W, min<int16_t>(ROW_H, LIST_END - rowY));
}

// Letter strip: '#', A..Z; letters with no entries dimmed, the current one
// (the first visible row's) highlighted. Blank for lists without a jump table.
static int stripLetter(const Level &l, const JumpTable *j) {
  int current = -1;
  if (j) {
    for (int b = 0; b < 27; b++) {
      if (static_cast<uint32_t>(l.top) >= j->start[b] && static_cast<uint32_t>(l.top) < j->start[b + 1]) {
        current = b;
      }
    }
  }
  return current;
}

static int stripShown = -2;  // letter highlighted on screen (-1: none)

// The strip moves with the hardware-scrolled list, so every frame of a
// moving list re-sends it: it's kept composed here (5.8 KB) and sent in one
// or two transfers, rebuilt only when its letters change.
static bool nowOpen = false;  // Now Playing is up (its code is further down)
static uint16_t stripBuf[STRIP_W * LIST_AREA_H];
static int stripBuiltFor = -3;                 // highlighted letter it was built with
static const JumpTable *stripBuiltTable = nullptr;
static bool stripStale = true;                 // theme changed


// Sends `px` (w x h) to the list area at screen line y, wrapping like listBlit.
static void listPush(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
  if (nowOpen) return;  // lcdPushPixels ignores the mute
  const int16_t mem = (y - LIST_Y + listOffset) % LIST_H;
  const int16_t first = min<int16_t>(h, LIST_H - mem);
  lcdPushPixels(x, LIST_Y + mem, w, first, px);
  if (first < h) lcdPushPixels(x, LIST_Y, w, h - first, px + first * w);
}

static void buildStrip(const JumpTable *j, int current) {
  for (uint32_t i = 0; i < STRIP_W * LIST_AREA_H; i++) stripBuf[i] = pal.bg;
  if (j) {
    const int perChunk = 6;  // 6 letters x 7 px = 42 px, fits the canvas
    for (int first = 0; first < 27; first += perChunk) {
      const int n = min(perChunk, 27 - first);
      canvas->fillRect(0, 0, STRIP_W, n * STRIP_PITCH, pal.bg);
      for (int k = 0; k < n; k++) {
        const int b = first + k;
        const char c = b == 0 ? '#' : static_cast<char>('A' + b - 1);
        const uint16_t color = b == current ? pal.accent : j->start[b] < j->start[b + 1] ? pal.faint : pal.line;
        canvas->drawChar(4, k * STRIP_PITCH, c, color, color, 1);
      }
      const uint16_t *src = canvas->getBuffer();
      for (int line = 0; line < n * STRIP_PITCH; line++) {
        memcpy(stripBuf + (STRIP_TOP + first * STRIP_PITCH + line) * STRIP_W, src + line * canvas->width(),
               STRIP_W * sizeof(uint16_t));
      }
    }
  }
  stripBuiltFor = current;
  stripBuiltTable = j;
  stripStale = false;
}

static void drawStrip() {
  const Level &l = level();
  const JumpTable *j = levelJump(l);
  const int current = stripLetter(l, j);
  stripShown = current;
  if (stripStale || current != stripBuiltFor || j != stripBuiltTable) buildStrip(j, current);
  listPush(LIST_W, LIST_Y, STRIP_W, LIST_AREA_H, stripBuf);
}

// Redraws the strip only when the highlighted letter changed.
static void updateStrip() {
  const Level &l = level();
  if (stripLetter(l, levelJump(l)) != stripShown) {
    drawStrip();
  }
}

static void drawRows() {
  for (int r = 0; r < VISIBLE_ROWS; r++) {
    drawRow(r);
  }
}

static void drawList() {
  if (!hasHero(level())) heroFree();
  drawRows();
  drawStrip();
}

// ================================================================ Swipe scrolling
// The list follows the finger pixel by pixel; a flick keeps it coasting with
// friction. It always comes to rest on a row boundary, so taps, the letter
// strip, page buttons and the playing row's marquee work in whole rows.
enum Motion : uint8_t {
  M_IDLE,   // at rest
  M_PRESS,  // finger down in the list, not moved far enough to be a drag
  M_DRAG,   // list follows the finger
  M_FLING,  // coasting after a flick
  M_SNAP,   // settling onto the nearest row
};

#define DRAG_SLOP_PX 8        // finger travel before a press becomes a drag
#define FLING_MIN_V 0.25f     // px/ms at release to start a flick
#define FLING_STOP_V 0.06f    // px/ms where coasting ends and the list snaps
#define FLING_MAX_V 30.0f     // px/ms (about one row per ms)
#define FLING_DECAY 0.998f    // speed kept per ms of coasting
#define FLING_STACK_MS 600    // a flick this soon after catching one adds its speed
#define VELOCITY_WINDOW_MS 120
#define FRAME_MS 15

static Motion motion = M_IDLE;
static int32_t scrollPx = 0;     // first visible pixel line of the list content
static int32_t scrollMaxPx = 0;
static int32_t scrollCount = 0;  // rows in the list being scrolled
static int32_t snapTarget = 0;
static bool scrollDirty = false;
static int16_t pressY = 0, dragLastY = 0;
static bool pressTaps = false;   // false when the press caught a moving list
static float flingV = 0;         // px/ms; positive moves toward later rows
static float caughtV = 0;        // speed of the flick the press caught
static uint32_t pressAt = 0, lastFrame = 0;

struct TouchSample {
  uint32_t t;
  int16_t y;
};
#define SAMPLES 6
static TouchSample samples[SAMPLES];
static uint8_t sampleCount = 0, sampleNext = 0;

static void addSample(int16_t y) {
  samples[sampleNext] = {millis(), y};
  sampleNext = (sampleNext + 1) % SAMPLES;
  if (sampleCount < SAMPLES) sampleCount++;
}

// Finger speed over the last moments before release, in content px/ms.
static float releaseVelocity() {
  if (sampleCount < 2) return 0;
  const TouchSample &last = samples[(sampleNext + SAMPLES - 1) % SAMPLES];
  // A poll after the last move saw the finger resting: it stopped, then lifted.
  // (Slow frames can delay polls; without such a poll it was still moving.)
  if (static_cast<int32_t>(touchLastStillPoll() - last.t) > 0) return 0;
  const TouchSample *first = nullptr;
  for (int k = sampleCount - 1; k >= 1; k--) {  // oldest sample inside the window
    const TouchSample &s = samples[(sampleNext + SAMPLES - 1 - k) % SAMPLES];
    if (last.t - s.t <= VELOCITY_WINDOW_MS) {
      first = &s;
      break;
    }
  }
  if (!first) {  // slow frames spaced the samples out: use the previous one
    first = &samples[(sampleNext + SAMPLES - 2) % SAMPLES];
  }
  if (last.t == first->t) return 0;
  return static_cast<float>(first->y - last.y) / static_cast<float>(last.t - first->t);
}

// Rows fetched while the list moves (each frame shows mostly the same rows).
#define ROW_CACHE 16
static Row rowCache[ROW_CACHE];
static int32_t rowCacheIndex[ROW_CACHE];

static void clearRowCache() {
  for (int i = 0; i < ROW_CACHE; i++) {
    rowCacheIndex[i] = -1;
    rowCache[i] = Row();
  }
}

static const Row *cachedRow(int32_t index) {
  if (index < 0 || index >= scrollCount) return nullptr;
  const int slot = index % ROW_CACHE;
  if (rowCacheIndex[slot] != index) {
    rowCache[slot] = Row();
    rowCacheIndex[slot] = levelRow(level(), index, rowCache[slot]) ? index : -1;
  }
  return rowCacheIndex[slot] == index ? &rowCache[slot] : nullptr;
}

// True while the list may be off a row boundary (including a press that
// caught a moving list).
static bool listMoving() {
  return motion != M_IDLE && !(motion == M_PRESS && pressTaps);
}

static int32_t shownPx = 0;  // scroll position currently on screen
static uint32_t movingFrames = 0, movingFrameMs = 0;  // for the Serial log

// Draws list lines from .. from+n-1 (0 = top of the list area) for the
// content at scroll position px, cutting rows where the band starts and ends.
static void drawListBand(int32_t px, int16_t from, int16_t n) {
  int32_t line = px + from;  // content line
  const int32_t end = line + n;
  while (line < end) {
    const int32_t index = line / ROW_H;
    const int16_t inRow = line % ROW_H;
    const int16_t h = min<int32_t>(ROW_H - inRow, end - line);
    composeRow(index, cachedRow(index), -1);
    listBlit(0, LIST_Y + (line - px), LIST_W, h, inRow);
    line += h;
  }
}

// Shows the list at scrollPx: the panel shifts the list area and only the
// lines that came into view are drawn (all of it after a big jump).
static uint32_t firstFrameAt = 0;

static void drawMovingList() {
  const uint32_t t0 = millis();
  thumbLookups = THUMB_LOOKUPS_PER_FRAME;
  if (movingFrames == 0) firstFrameAt = t0;
  const int32_t delta = scrollPx - shownPx;
  if (delta >= LIST_H || delta <= -LIST_H) {
    drawListBand(scrollPx, 0, LIST_H);
  } else if (delta != 0) {
    setListOffset(listOffset + delta);
    if (delta > 0) {
      drawListBand(scrollPx, LIST_H - delta, delta);
    } else {
      drawListBand(scrollPx, 0, -delta);
    }
  }
  shownPx = scrollPx;
  level().top = scrollPx / ROW_H;
  if (levelJump(level())) {
    drawStrip();  // the panel shifted the letters too; a plain strip needs nothing
  }
  movingFrames++;
  movingFrameMs += millis() - t0;
}

// Takes the list's rest position as the scroll origin.
static void beginScroll() {
  scrollCount = levelCount(level());
  scrollPx = shownPx = level().top * ROW_H;
  // Normally the last page ends at the last row; page down can leave the
  // list further on, so allow staying there.
  scrollMaxPx = max<int32_t>(max<int32_t>(0, scrollCount - FULL_ROWS) * ROW_H, scrollPx);
  clearRowCache();
}

// Moves the list; returns false if it hit an end.
static bool setScroll(int32_t px) {
  const int32_t clamped = constrain(px, 0, scrollMaxPx);
  if (clamped != scrollPx) {
    scrollPx = clamped;
    scrollDirty = true;
  }
  return clamped == px;
}

static void startSnap() {
  snapTarget = constrain((scrollPx + ROW_H / 2) / ROW_H * ROW_H, 0, scrollMaxPx / ROW_H * ROW_H);
  motion = M_SNAP;
  lastFrame = millis();
}

// At rest on a row: normal drawing (marquee for the playing row).
static void settleList() {
  motion = M_IDLE;
  scrollDirty = false;
  level().top = scrollPx / ROW_H;
  if (shownPx != scrollPx) {
    drawRows();  // not drawn at this position yet
  } else {
    for (int r = 0; r < VISIBLE_ROWS; r++) {  // the screen is right; restart the
      const Row *row = cachedRow(level().top + r);  // playing row's marquee
      if (row && row->kind == R_SONG && isPlaying(row->id)) {
        drawRow(r);
      }
    }
  }
  shownPx = scrollPx;
  if (thumbPending) {  // rows drawn while moving left placeholders
    thumbPending = false;
    drawRows();
  }
  updateStrip();
  if (movingFrames) {
    const uint32_t total = millis() - firstFrameAt;
    Serial.printf("[scroll] rest at row %ld: %lu frames in %lu ms (%lu ms per frame, %lu drawing), "
                  "audio underruns %lu\n",
                  level().top, movingFrames, total, total / movingFrames,
                  movingFrameMs / movingFrames, audioUnderruns());
    movingFrames = movingFrameMs = 0;
  }
}

// Ends any movement at once on the nearest row (before navigation, buttons...).
static void stopList() {
  if (listMoving()) {
    scrollPx = constrain((scrollPx + ROW_H / 2) / ROW_H * ROW_H, 0, scrollMaxPx);
    settleList();
  } else {
    motion = M_IDLE;
  }
}

// Rows' content changed (e.g. the playing highlight moved).
static void refreshRows() {
  if (listMoving()) {
    shownPx = scrollPx + 2 * LIST_H;  // next frame redraws the whole list
    scrollDirty = true;
  } else {
    drawRows();
  }
}

static void tickList() {
  if (motion == M_IDLE || motion == M_PRESS) return;
  const uint32_t now = millis();
  if (motion == M_FLING || motion == M_SNAP) {
    const uint32_t dt = now - lastFrame;
    if (dt < FRAME_MS) return;
    lastFrame = now;
    if (motion == M_FLING) {
      // Exact distance under exponential friction over dt (frames vary).
      const uint32_t step = min<uint32_t>(dt, 200);
      const float keep = powf(FLING_DECAY, static_cast<float>(step));
      const float dist = flingV * (1.0f - keep) / -logf(FLING_DECAY);
      flingV *= keep;
      if (!setScroll(scrollPx + lroundf(dist))) {
        flingV = 0;  // hit an end
      }
      if (fabsf(flingV) < FLING_STOP_V) {
        startSnap();
      }
    } else {
      const int32_t d = snapTarget - scrollPx;
      int32_t move = d / 2;
      if (abs(move) < 3) move = constrain(d, -3, 3);
      setScroll(scrollPx + move);
      if (scrollPx == snapTarget) {
        settleList();
        return;
      }
    }
  }
  if (scrollDirty) {
    scrollDirty = false;
    drawMovingList();
  }
}

static void tapRow(int r);

static int16_t pressX = 0;

static void listTouchDown(int16_t x, int16_t y) {
  pressX = x;
  const bool moving = motion == M_FLING || motion == M_SNAP;
  caughtV = motion == M_FLING ? flingV : 0;
  if (!moving) {
    beginScroll();
  }
  motion = M_PRESS;
  pressTaps = !moving;  // touching a moving list just stops it
  pressY = dragLastY = y;
  pressAt = millis();
  sampleCount = sampleNext = 0;
  addSample(y);
}

static void listTouchMove(int16_t y) {
  if (motion == M_PRESS && abs(y - pressY) > DRAG_SLOP_PX) {
    motion = M_DRAG;
    listMarquee.active = false;
  }
  if (motion == M_DRAG) {
    setScroll(scrollPx + (dragLastY - y));
    dragLastY = y;
    addSample(y);
  }
}

static void listTouchUp() {
  if (motion == M_PRESS) {
    if (pressTaps) {
      motion = M_IDLE;
      tapRow((pressY - LIST_Y) / ROW_H);
    } else {
      startSnap();  // caught mid-flick: settle where it stopped
    }
    return;
  }
  if (motion != M_DRAG) return;
  float v = releaseVelocity();
  if (fabsf(v) < FLING_MIN_V) {
    startSnap();
    return;
  }
  // Quick repeated flicks the same way build up speed.
  if (caughtV != 0 && (caughtV > 0) == (v > 0) && millis() - pressAt < FLING_STACK_MS) {
    v += caughtV;
  }
  flingV = constrain(v, -FLING_MAX_V, FLING_MAX_V);
  motion = M_FLING;
  lastFrame = millis();
}

// Elapsed time estimated from the decoder's position within the audio bytes.
static uint32_t elapsedMs() {
  if (playIndex < 0) {
    return 0;
  }
  const uint32_t pos = audioFilePos();
  if (current.durationMs == 0 || current.audioEnd <= current.audioStart ||
      pos <= current.audioStart) {
    return 0;
  }
  const uint64_t done = min(pos, current.audioEnd) - current.audioStart;
  return done * current.durationMs / (current.audioEnd - current.audioStart);
}

// Position in the file as a fraction of `width` pixels.
static int progressPx(int width) {
  const uint32_t size = audioFileSize();
  if (playIndex < 0 || size == 0) return 0;
  return constrain(static_cast<int>(static_cast<uint64_t>(audioFilePos()) * width / size), 0, width);
}

// The playing song's cover thumbnail (nullptr: none).
static const uint16_t *currentThumb() {
  if (playIndex < 0 || current.album.isEmpty()) return nullptr;
  const String artist = current.albumArtist.length() ? current.albumArtist
                        : current.artist.length()    ? current.artist
                                                     : String("Unknown Artist");
  bool missing = true;
  const uint16_t *px = thumbFor(artAlbumKey(artist.c_str(), current.album.c_str()), true, &missing);
  if (!px && missing && current.albumArtist.isEmpty()) {
    px = thumbFor(artAlbumKey("Various Artists", current.album.c_str()), true, &missing);
  }
  return px;
}

// ---------------------------------------------------------------- Mini player
// Above the tabs: a progress line, the cover, title and artist, play/pause
// and next. Tapping the rest opens Now Playing.
#define MINI_PLAY_X 176
#define MINI_NEXT_X 206

static void drawMini() {
  canvas->fillRect(0, 0, SCREEN_W, MINI_H, pal.surface);
  const int px = progressPx(SCREEN_W);
  canvas->fillRect(0, 0, SCREEN_W, 2, pal.line);
  canvas->fillRect(0, 0, px, 2, pal.accent);
  lastProgressPx = px;
  if (playIndex < 0) {
    uiDrawText(FONT_SMALL, Catalog::songCount() ? "Tap a song to play" : "No MP3 files found", 10, 26, pal.dim);
  } else {
    const uint16_t *art = currentThumb();
    if (art) {
      canvas->drawRGBBitmap(8, 7, art, ART_THUMB, ART_THUMB);
    } else {
      canvas->fillRect(8, 7, ART_THUMB, ART_THUMB, pal.surface2);
      uiDrawIcon(ICONS_18, ICON_MUSIC_NOTE, 14, 13, pal.faint);
    }
    roundCorners(8, 7, ART_THUMB, uiRadiusArt(), pal.surface);
    const int16_t textW = MINI_PLAY_X - 4 - 46;
    uiDrawText(FONT_BODY, uiFitText(FONT_BODY, current.title, textW).c_str(), 46, 21, pal.text);
    const String artist = current.artist.isEmpty() ? "Unknown artist" : current.artist;
    uiDrawText(FONT_SMALL, uiFitText(FONT_SMALL, artist, textW).c_str(), 46, 34, pal.dim);
    uiDrawIcon(ICONS_24, audioState() == PLAYING ? ICON_PAUSE : ICON_PLAY, MINI_PLAY_X + 2, 10, pal.text);
    uiDrawIcon(ICONS_24, ICON_NEXT, MINI_NEXT_X + 4, 10, pal.text);
  }
  lcdBlit(0, MINI_Y, SCREEN_W, MINI_H);
}

// Moves the mini player's progress line (drawn straight to the screen).
static void drawMiniProgress(bool force) {
  const int px = progressPx(SCREEN_W);
  if (!force && px == lastProgressPx) return;
  if (force || px < lastProgressPx) {
    lcdFillRect(px, MINI_Y, SCREEN_W - px, 2, pal.line);
    lcdFillRect(0, MINI_Y, px, 2, pal.accent);
  } else {
    lcdFillRect(lastProgressPx, MINI_Y, px - lastProgressPx, 2, pal.accent);
  }
  lastProgressPx = px;
}

void uiShowStatus(const char *line1, const char *line2) {
  dropNow();
  resetListScroll();
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  drawCenteredText(line1, SCREEN_W / 2, 6, 2, C_TEXT);
  drawCenteredText(line2, SCREEN_W / 2, 30, 1, C_DIM);
  lcdBlit(0, 130, SCREEN_W, CANVAS_H);
}

// Clears the screen under a plain "MP3 Player" header.
static void drawPlainScreen() {
  dropNow();
  resetListScroll();
  lcdFillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  canvas->fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
  uiDrawText(FONT_TITLE, "MP3 Player", 10, 22, pal.text);
  lcdBlit(0, 0, SCREEN_W, HEADER_H);
}

void uiShowMessage(const char *line1, const char *line2) {
  drawPlainScreen();
  uiShowStatus(line1, line2);
}

#define PROGRESS_TITLE_Y 62
#define PROGRESS_SCREEN_Y 104
#define FOLDER_Y 160
#define BAR_X 20
#define BAR_W (SCREEN_W - 2 * BAR_X)

void uiShowProgress(bool redraw, const char *title, const char *phase, float fraction,
                    const String &counts, const char *folder, const char *hint) {
  if (redraw) {
    drawPlainScreen();
    canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
    drawCenteredText(title, SCREEN_W / 2, 6, 2, C_TEXT);
    lcdBlit(0, PROGRESS_TITLE_Y, SCREEN_W, CANVAS_H);
    canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
    canvas->setTextSize(1);
    canvas->setTextColor(C_DIM);
    // Hint: up to two lines, split at '\n'.
    const String h = hint;
    const int nl = h.indexOf('\n');
    drawCenteredText(nl < 0 ? h : h.substring(0, nl), SCREEN_W / 2, 10, 1, C_DIM);
    if (nl >= 0) {
      drawCenteredText(h.substring(nl + 1), SCREEN_W / 2, 22, 1, C_DIM);
    }
    lcdBlit(0, SCREEN_H - CANVAS_H, SCREEN_W, CANVAS_H);
  }
  // Phase, bar, counts.
  fraction = constrain(fraction, 0.0f, 1.0f);
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  drawCenteredText(phase, SCREEN_W / 2, 0, 1, C_TEXT);
  canvas->fillRect(BAR_X, 15, BAR_W, 6, pal.line);
  canvas->fillRect(BAR_X, 15, static_cast<int16_t>(BAR_W * fraction), 6, pal.accent);
  roundRect(BAR_X, 15, BAR_W, 6, uiStyle.square ? 0 : 3, pal.bg);
  drawCenteredText(counts + (counts.isEmpty() ? "" : "   ") + String(static_cast<int>(fraction * 100)) + "%",
                   SCREEN_W / 2, 32, 1, C_DIM);
  lcdBlit(0, PROGRESS_SCREEN_Y, SCREEN_W, CANVAS_H);
  // Folder being worked on: two lines, trimmed from the left.
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  if (folder && folder[0]) {
    const String converted = decodeUtf8(reinterpret_cast<const uint8_t *>(folder), strlen(folder));
    // Two lines of the path, its end kept: the first line takes what fits.
    const int16_t room = SCREEN_W - 20;
    String rest = converted;
    while (uiTextWidth(FONT_SMALL, rest.c_str()) > 2 * room && rest.length() > 1) rest.remove(0, 1);
    size_t cut = rest.length();
    while (cut > 0 && uiTextWidth(FONT_SMALL, rest.substring(0, cut).c_str()) > room) cut--;
    uiDrawText(FONT_LABEL, "FOLDER", 10, 10, pal.dim);
    uiDrawText(FONT_SMALL, rest.substring(0, cut).c_str(), 10, 25, pal.text);
    uiDrawText(FONT_SMALL, rest.substring(cut).c_str(), 10, 39, pal.text);
  }
  lcdBlit(0, FOLDER_Y, SCREEN_W, CANVAS_H);
}

void uiShowPlayer() {
  dropNow();
  static bool initialized = false;
  if (!initialized) {
    resetTabs();
    loadPlayModes();
    initialized = true;
  }
  if (!scrollAreaSet) {
    lcdScrollArea(LIST_Y, LIST_H);
    listOffset = 0;
    scrollAreaSet = true;
  }
  String pass;
  if (!wifiSavedCredentials(wifiSsid, pass)) wifiSsid = "";
  motion = M_IDLE;
  drawHeader();
  drawList();
  drawMini();
  drawTabs();
}

// ================================================================ Now Playing
// Full screen: cover, title, seek bar, controls, volume. The browse screen's
// own updates keep running underneath; the LCD is muted for them while this
// is up and unmuted only for this screen's drawing.
static uint32_t nowSongId = LIB_NONE;
static bool volumeDrag = false;
static String playFromTitle;  // the list playback started from

#define NP_HEADER_H 30
#define NP_COVER 125  // a 250 px cover at 1/2
#define NP_COVER_Y 34
#define NP_TEXT_Y 163
#define NP_TEXT_H 40
#define NP_BAR_Y 203
#define NP_BAR_H 28
#define NP_BAR_X 16
#define NP_BAR_W (SCREEN_W - 2 * NP_BAR_X)
#define NP_CTRL_Y 231
#define NP_CTRL_H 48  // every band must fit the canvas (CANVAS_H)
#define NP_VOL_Y (NP_CTRL_Y + NP_CTRL_H)  // 279
#define NP_VOL_H (SCREEN_H - NP_VOL_Y)    // 41
#define NP_VOL_X 40
#define NP_VOL_W 150

static_assert(NP_HEADER_H <= CANVAS_H && NP_TEXT_H <= CANVAS_H && NP_BAR_H <= CANVAS_H && NP_CTRL_H <= CANVAS_H &&
                NP_VOL_H <= CANVAS_H && MINI_H <= CANVAS_H && TABBAR_H <= CANVAS_H && HEADER_H <= CANVAS_H,
              "every band is composed in the canvas");

struct NowDraw {  // unmutes the LCD for this screen's drawing
  NowDraw() { lcdSetMuted(false); }
  ~NowDraw() { lcdSetMuted(nowOpen); }
};

// The playing song's cover file ("" if none).
static String currentCoverPath() {
  if (current.album.isEmpty()) return "";
  const String artist = current.albumArtist.length() ? current.albumArtist
                        : current.artist.length()    ? current.artist
                                                     : String("Unknown Artist");
  FILINFO fi;
  String jpg = artCoverPath(artist.c_str(), current.album.c_str());
  if (f_stat(cardPath(jpg).c_str(), &fi) == FR_OK) return jpg;
  if (current.albumArtist.isEmpty()) {
    jpg = artCoverPath("Various Artists", current.album.c_str());
    if (f_stat(cardPath(jpg).c_str(), &fi) == FR_OK) return jpg;
  }
  return "";
}

static void drawNowText() {
  canvas->fillRect(0, 0, SCREEN_W, NP_TEXT_H, pal.bg);
  const UiFont &tf = FONT_HEADLINE;
  const int16_t base = 19, room = SCREEN_W - 32;
  if (uiTextWidth(tf, current.title.c_str()) <= room) {
    titleMarquee.active = false;
    drawCentered(tf, current.title, SCREEN_W / 2, base, pal.text);
  } else {
    marqueeSet(titleMarquee, current.title, 16, NP_TEXT_Y + base - tf.ascent, room, tf, pal.text, pal.bg);
    marqueeCompose(titleMarquee, 16, base - tf.ascent);
  }
  const String artist = current.artist.isEmpty() ? "Unknown artist" : current.artist;
  drawCentered(FONT_SMALL, uiFitText(FONT_SMALL, artist, room), SCREEN_W / 2, 35, pal.dim);
  lcdBlit(0, NP_TEXT_Y, SCREEN_W, NP_TEXT_H);
}

static void drawNowBar() {
  canvas->fillRect(0, 0, SCREEN_W, NP_BAR_H, pal.bg);
  const int px = progressPx(NP_BAR_W);
  canvas->fillRect(NP_BAR_X, 6, NP_BAR_W, 3, pal.line);
  canvas->fillRect(NP_BAR_X, 6, px, 3, pal.accent);
  drawDot(NP_BAR_X + px, 7, 5, pal.text, pal.bg);
  const String at = formatTime(elapsedMs());
  uiDrawText(FONT_SMALL, at.c_str(), NP_BAR_X, 25, pal.dim);
  if (current.durationMs) {
    const String total = formatTime(current.durationMs);
    uiDrawText(FONT_SMALL, total.c_str(), NP_BAR_X + NP_BAR_W - uiTextWidth(FONT_SMALL, total.c_str()), 25, pal.dim);
  }
  lastTimeShown = elapsedMs() / 1000;
  lcdBlit(0, NP_BAR_Y, SCREEN_W, NP_BAR_H);
}

static void drawNowControls() {
  canvas->fillRect(0, 0, SCREEN_W, NP_CTRL_H, pal.bg);
  const int16_t cy = NP_CTRL_H / 2;
  uiDrawIcon(ICONS_24, ICON_PREVIOUS, 54, cy - 12, pal.text);
  drawDot(SCREEN_W / 2, cy, 23, pal.accent, pal.bg);
  uiDrawIcon(ICONS_24, audioState() == PLAYING ? ICON_PAUSE : ICON_PLAY, SCREEN_W / 2 - 12, cy - 12, pal.onAccent);
  uiDrawIcon(ICONS_24, ICON_NEXT, SCREEN_W - 54 - 24, cy - 12, pal.text);
  uiDrawIcon(ICONS_18, ICON_SHUFFLE, 14, cy - 9, shuffleOn ? pal.accent : pal.faint);
  uiDrawIcon(ICONS_18, ICON_REPEAT, SCREEN_W - 14 - 18, cy - 9, repeatMode ? pal.accent : pal.faint);
  if (repeatMode == REPEAT_ONE) uiDrawText(FONT_LABEL, "1", SCREEN_W - 14 - 2, cy + 12, pal.accent);
  lcdBlit(0, NP_CTRL_Y, SCREEN_W, NP_CTRL_H);
}

static void drawNowVolume() {
  canvas->fillRect(0, 0, SCREEN_W, NP_VOL_H, pal.bg);
  const int16_t cy = 20;
  uiDrawIcon(ICONS_18, ICON_VOLUME, 12, cy - 9, pal.dim);
  const int16_t px = audioVolume() * NP_VOL_W / VOLUME_STEPS;
  canvas->fillRect(NP_VOL_X, cy - 1, NP_VOL_W, 3, pal.line);
  canvas->fillRect(NP_VOL_X, cy - 1, px, 3, pal.text);
  drawDot(NP_VOL_X + px, cy, 6, pal.text, pal.bg);
  const String level = String(audioVolume());
  uiDrawText(FONT_SMALL, level.c_str(), NP_VOL_X + NP_VOL_W + 16, cy + 4, pal.dim);
  lcdBlit(0, NP_VOL_Y, SCREEN_W, NP_VOL_H);
}

// ---------------------------------------------------------------- Lyrics
// From an .lrc file next to the song ("Song.mp3" -> "Song.lrc"): timed
// lines "[mm:ss.xx]text" (several stamps per line, [offset:ms] honoured),
// or plain text, which scrolls with the song's progress. Lines are wrapped
// to the screen once, when the song starts. Shown in Now Playing's upper
// part instead of the cover; a tap on a timed line jumps there.
#define LYRICS_MAX_FILE 12288
#define LYRICS_MAX_LINES 400
#define LYRIC_H 22
#define LYRICS_Y NP_HEADER_H
#define LYRICS_H (NP_BAR_Y - NP_HEADER_H)  // 173
#define LYRICS_W (SCREEN_W - 32)
struct LyricLine {
  uint32_t ms;
  uint16_t text;  // offset in lyricText
};
static char *lyricText = nullptr;
static LyricLine *lyricLines = nullptr;
static uint16_t lyricCount = 0;
static bool lyricSynced = false;
static uint32_t lyricSongId = LIB_NONE;
static bool nowLyrics = false;  // Now Playing shows the lyrics
static int lyricShown = -2, lyricFirst = 0;

static void lyricsFree() {
  free(lyricText);
  free(lyricLines);
  lyricText = nullptr;
  lyricLines = nullptr;
  lyricCount = 0;
}

static bool validUtf8(const char *s, size_t n) {
  for (size_t i = 0; i < n;) {
    const uint8_t c = s[i];
    const int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || i + extra >= n + (extra ? 0 : 1)) return false;
    for (int k = 1; k <= extra; k++) {
      if ((static_cast<uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += 1 + extra;
  }
  return true;
}

// "mm:ss", "mm:ss.xx" or "mm:ss:xx" -> ms; false if not a time.
static bool parseStamp(const char *s, size_t n, uint32_t &ms) {
  uint32_t min = 0, sec = 0, frac = 0, fracDigits = 0;
  size_t i = 0;
  while (i < n && isdigit(static_cast<unsigned char>(s[i]))) min = min * 10 + (s[i++] - '0');
  if (i == 0 || i >= n || s[i++] != ':') return false;
  const size_t secStart = i;
  while (i < n && isdigit(static_cast<unsigned char>(s[i]))) sec = sec * 10 + (s[i++] - '0');
  if (i == secStart) return false;
  if (i < n && (s[i] == '.' || s[i] == ':')) {
    i++;
    while (i < n && isdigit(static_cast<unsigned char>(s[i])) && fracDigits < 3) frac = frac * 10 + (s[i++] - '0'), fracDigits++;
  }
  if (i != n) return false;
  while (fracDigits < 3) frac *= 10, fracDigits++;
  ms = (min * 60 + sec) * 1000 + frac;
  return true;
}

// Loads and wraps the playing song's lyrics (none: lyricCount 0).
static void lyricsLoad() {
  lyricsFree();
  lyricSongId = current.id;
  lyricShown = -2;
  if (playIndex < 0) return;
  const int dot = currentPath.lastIndexOf('.');
  if (dot < 0) return;
  FIL *fil = cardNewFil();
  if (!fil) return;
  bool open = false;
  for (const char *ext : {".lrc", ".LRC"}) {
    if (f_open(fil, cardPath(currentPath.substring(0, dot) + ext).c_str(), FA_READ) == FR_OK) {
      open = true;
      break;
    }
  }
  if (!open) {
    cardDeleteFil(fil);
    return;
  }
  const UINT size = min<UINT>(f_size(fil), LYRICS_MAX_FILE);
  char *raw = static_cast<char *>(malloc(size + 1));
  UINT got = 0;
  if (raw && f_read(fil, raw, size, &got) == FR_OK) raw[got] = 0;
  else got = 0;
  f_close(fil);
  cardDeleteFil(fil);
  if (!got) {
    free(raw);
    return;
  }
  // Text as UTF-8: Latin-1 files converted.
  String text;
  if (validUtf8(raw, got)) {
    text = raw;
  } else {
    text.reserve(got + got / 8);
    for (UINT i = 0; i < got; i++) {
      const uint8_t c = raw[i];
      if (c < 0x80) text += static_cast<char>(c);
      else text += static_cast<char>(0xC0 | (c >> 6)), text += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  free(raw);
  if (text.startsWith("\xEF\xBB\xBF")) text.remove(0, 3);  // byte order mark

  // Pass 1: entries (time, text) in file order.
  struct Entry {
    uint32_t ms;
    String text;
  };
  std::vector<Entry> entries;
  int32_t offsetMs = 0;
  bool timed = false;
  int start = 0;
  while (start <= static_cast<int>(text.length()) && entries.size() < LYRICS_MAX_LINES) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String line = text.substring(start, end);
    start = end + 1;
    line.trim();
    std::vector<uint32_t> stamps;
    bool tag = false;
    while (line.startsWith("[")) {
      const int close = line.indexOf(']');
      if (close < 0) break;
      const String inside = line.substring(1, close);
      uint32_t ms;
      if (parseStamp(inside.c_str(), inside.length(), ms)) {
        stamps.push_back(ms);
      } else {
        if (inside.startsWith("offset:")) offsetMs = inside.substring(7).toInt();
        tag = true;
      }
      line = line.substring(close + 1);
      line.trim();
    }
    if (!stamps.empty()) {
      timed = true;
      for (uint32_t ms : stamps) entries.push_back({ms, line});
    } else if (!tag && line.length()) {
      entries.push_back({0, line});
    }
  }
  if (entries.empty()) return;
  lyricSynced = timed;
  if (timed) {  // stamps can appear in any order (a chorus listed once with several stamps)
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.ms < b.ms; });
    for (Entry &e : entries) e.ms = static_cast<uint32_t>(max<int32_t>(0, static_cast<int32_t>(e.ms) - offsetMs));
  }
  // Pass 2: wrap to the screen into one buffer.
  String wrapped;
  std::vector<LyricLine> lines;
  for (const Entry &e : entries) {
    String rest = e.text;
    do {
      size_t cut = rest.length();
      while (cut > 0 && uiTextWidth(FONT_BODY, rest.substring(0, cut).c_str()) > LYRICS_W) {
        const int space = rest.lastIndexOf(' ', cut - 1);
        cut = space > 0 ? space : cut - 1;
      }
      if (cut == 0) cut = rest.length();
      lines.push_back({e.ms, static_cast<uint16_t>(wrapped.length())});
      wrapped += rest.substring(0, cut);
      wrapped += '\0';
      rest = rest.substring(cut);
      rest.trim();
    } while (rest.length() && lines.size() < LYRICS_MAX_LINES);
    if (lines.size() >= LYRICS_MAX_LINES) break;
  }
  lyricText = static_cast<char *>(malloc(wrapped.length() + 1));
  lyricLines = static_cast<LyricLine *>(malloc(lines.size() * sizeof(LyricLine)));
  if (!lyricText || !lyricLines) {
    lyricsFree();
    return;
  }
  memcpy(lyricText, wrapped.c_str(), wrapped.length() + 1);
  for (size_t i = 0; i < wrapped.length(); i++) {
    if (wrapped[i] == 0) lyricText[i] = 0;  // String keeps the embedded NULs
  }
  memcpy(lyricLines, lines.data(), lines.size() * sizeof(LyricLine));
  lyricCount = lines.size();
  Serial.printf("[lyrics] %u lines (%s), %u bytes\n", lyricCount, lyricSynced ? "timed" : "plain", wrapped.length());
}

// The line playing now.
static int currentLyric() {
  if (!lyricCount) return -1;
  const uint32_t at = elapsedMs();
  if (!lyricSynced) {
    return current.durationMs ? min<int>(lyricCount - 1, static_cast<uint64_t>(at) * lyricCount / current.durationMs) : 0;
  }
  int cur = -1;
  for (int i = 0; i < lyricCount && lyricLines[i].ms <= at; i++) cur = i;
  return cur;
}

static void drawLyrics(bool force) {
  const int cur = currentLyric();
  if (!force && cur == lyricShown) return;
  lyricShown = cur;
  const int perScreen = LYRICS_H / LYRIC_H;  // 7
  lyricFirst = max(0, cur - perScreen / 2 + 1);
  for (int16_t band = 0; band < LYRICS_H; band += 2 * LYRIC_H) {
    const int16_t h = min<int16_t>(2 * LYRIC_H, LYRICS_H - band);
    canvas->fillRect(0, 0, SCREEN_W, h, pal.bg);
    if (!lyricCount) {
      if (band == 2 * LYRIC_H) {
        drawCentered(FONT_BODY, "No lyrics for this song", SCREEN_W / 2, 16, pal.dim);
        drawCentered(FONT_SMALL, "An .lrc file next to the MP3 shows here", SCREEN_W / 2, 34, pal.faint);
      }
    } else {
      for (int k = 0; k < 2; k++) {
        const int i = lyricFirst + band / LYRIC_H + k;
        if (i >= lyricCount) break;
        const uint16_t color = i == cur ? pal.accent : i < cur ? pal.faint : pal.dim;
        uiDrawText(FONT_BODY, lyricText + lyricLines[i].text, 16, k * LYRIC_H + 16, color);
      }
    }
    lcdBlit(0, LYRICS_Y + band, SCREEN_W, h);
  }
}

// Jumps to ms in the playing song (byte position estimated from the length).
static void seekTo(uint32_t ms) {
  if (playIndex < 0 || !current.durationMs || current.audioEnd <= current.audioStart) return;
  ms = min(ms, current.durationMs - 1000);
  const uint32_t pos = current.audioStart +
                       static_cast<uint64_t>(current.audioEnd - current.audioStart) * ms / current.durationMs;
  audioPlay(currentPath.c_str(), pos);
  lyricShown = -2;
}

static void drawNow() {
  NowDraw unmute;
  nowSongId = current.id;
  setListOffset(0);  // this screen covers the hardware-scrolled list area
  // Header: collapse, where playback came from.
  canvas->fillRect(0, 0, SCREEN_W, NP_HEADER_H, pal.bg);
  uiDrawIcon(ICONS_24, ICON_EXPAND, 6, 3, pal.text);
  drawCentered(FONT_TAB, "Playing from", SCREEN_W / 2, 12, pal.dim);
  drawCentered(FONT_LABEL, uiFitText(FONT_LABEL, playFromTitle, SCREEN_W - 96), SCREEN_W / 2, 25, pal.text);
  if (lyricSongId != current.id) lyricsLoad();
  uiDrawIcon(ICONS_24, ICON_LYRICS, SCREEN_W - 30, 3, nowLyrics ? pal.accent : lyricCount ? pal.text : pal.faint);
  lcdBlit(0, 0, SCREEN_W, NP_HEADER_H);
  if (nowLyrics) {
    titleMarquee.active = false;
    drawLyrics(true);
    drawNowBar();
    drawNowControls();
    drawNowVolume();
    return;
  }
  // Cover.
  lcdFillRect(0, NP_HEADER_H, SCREEN_W, NP_TEXT_Y - NP_HEADER_H, pal.bg);
  const int16_t coverX = (SCREEN_W - NP_COVER) / 2;
  const String jpg = currentCoverPath();
  ArtDrawResult art;
  if (jpg.length()) art = artDrawJpeg(cardPath(jpg), coverX, NP_COVER_Y, NP_COVER, NP_COVER);
  if (!art.ok) {
    lcdFillRect(coverX, NP_COVER_Y, NP_COVER, NP_COVER, pal.surface2);
    canvas->fillRect(0, 0, NP_COVER, 24, pal.surface2);
    uiDrawIcon(ICONS_24, ICON_MUSIC_NOTE, (NP_COVER - 24) / 2, 0, pal.faint);
    lcdBlit(coverX, NP_COVER_Y + (NP_COVER - 24) / 2, NP_COVER, 24);
  }
  drawNowText();
  drawNowBar();
  drawNowControls();
  drawNowVolume();
}

static void openNow() {
  stopList();
  listMarquee.active = false;
  nowOpen = true;
  drawNow();
}

// Ends Now Playing without drawing (another screen takes over).
static void dropSearch();
static void dropNow() {
  dropSearch();
  if (nowOpen) {
    nowOpen = false;
    titleMarquee.active = false;
    lcdSetMuted(false);
    lcdScrollOffset(listOffset);  // may have moved while muted
  }
}

static void closeNow() {
  dropNow();
  uiShowPlayer();
}

static void setVolumeAt(int16_t x) {
  const int level = constrain((x - NP_VOL_X + NP_VOL_W / VOLUME_STEPS / 2) * VOLUME_STEPS / NP_VOL_W, 0, VOLUME_STEPS);
  if (level != audioVolume()) {
    audioSetVolume(level);
    NowDraw unmute;
    drawNowVolume();
  }
}

// The play state or the song changed: the mini player and Now Playing.
static void refreshPlaying() {
  drawMini();
  if (nowOpen) {
    if (current.id != nowSongId) {
      drawNow();
    } else {
      NowDraw unmute;
      drawNowControls();
    }
  }
}

// ================================================================ Actions
static void startPlayback() {
  uint32_t id;
  if (!ctxSong(playCtx, playIndex, id) || !Catalog::song(id, current)) {
    playIndex = -1;
    refreshPlaying();
    return;
  }
  audioStop();
  currentPath = Catalog::songPath(id);
  Serial.println("Playing " + currentPath);
  if (!audioPlay(currentPath.c_str(), current.audioStart)) {
    Serial.println("  could not start playback");
  }
  lastProgressPx = -1;
  refreshRows();  // move the playing highlight
  refreshPlaying();
}

static void playFromLevel(const Level &l, uint32_t songIndex) {
  playCtx = levelContext(l);
  playFromTitle = l.title;
  playIndex = songIndex;
  reshuffle(songIndex, playCtx.count);
  startPlayback();
}

static void togglePlayPause() {
  const PlayerState state = audioState();
  if (state == PLAYING) {
    audioPause();
  } else if (state == PAUSED) {
    audioResume();
  } else if (playIndex >= 0) {
    startPlayback();
    return;
  } else {
    // Nothing chosen yet: play this list from its first visible song.
    const Level &l = level();
    const PlayContext c = levelContext(l);
    if (c.count > 0) {
      const int32_t first = max<int32_t>(0, l.top - static_cast<int32_t>(songRowOffset(l)));
      playFromLevel(l, min<uint32_t>(first, c.count - 1));
    }
    return;
  }
  refreshPlaying();
}

// Moves playIndex to the next song (shuffled order if on). At the end of
// the list: wraps if `wrap`, else returns false.
static bool stepForward(bool wrap) {
  const uint32_t n = playCtx.count;
  if (playIndex < 0 || n == 0) return false;
  if (shuffleOn) {
    if (shufflePos + 1 >= n) {
      if (!wrap) return false;
      // A fresh order for the next round, not starting with the song that just played.
      const int32_t last = playIndex;
      reshuffle(esp_random() % n, n);
      if (n > 1 && static_cast<int32_t>(shuffledIndex(0, n)) == last) shufflePos = 1;
      playIndex = shuffledIndex(shufflePos, n);
      return true;
    }
    playIndex = shuffledIndex(++shufflePos, n);
    return true;
  }
  if (static_cast<uint32_t>(playIndex) + 1 >= n) {
    if (!wrap) return false;
    playIndex = 0;
    return true;
  }
  playIndex++;
  return true;
}

static void stepBack() {
  const uint32_t n = playCtx.count;
  if (shuffleOn) {
    shufflePos = shufflePos == 0 ? n - 1 : shufflePos - 1;
    playIndex = shuffledIndex(shufflePos, n);
  } else {
    playIndex = playIndex == 0 ? n - 1 : playIndex - 1;
  }
}

static void nextTrack() {
  if (stepForward(true)) {
    startPlayback();
  }
}

static void previousTrack() {
  if (playIndex < 0 || playCtx.count == 0) {
    return;
  }
  // Restart the current song if we're more than a few seconds in.
  if (!(audioState() != STOPPED && audioFilePos() > 64 * 1024)) {
    stepBack();
  }
  startPlayback();
}

static void handleTrackEnded() {
  Serial.println("Track finished");
  if (repeatMode == REPEAT_ONE && playIndex >= 0) {
    startPlayback();
  } else if (stepForward(repeatMode == REPEAT_ALL)) {
    startPlayback();
  } else {
    refreshPlaying();
  }
}

static void toggleShuffle() {
  shuffleOn = !shuffleOn;
  if (shuffleOn && playIndex >= 0) reshuffle(playIndex, playCtx.count);  // continue from this song
  savePlayModes();
}

static void cycleRepeat() {
  repeatMode = (repeatMode + 1) % 3;
  savePlayModes();
}

static void scrollTo(int32_t top) {
  const int32_t count = levelCount(level());
  top = constrain(top, 0, max<int32_t>(0, count - 1));
  if (top != level().top) {
    level().top = top;
    drawList();
  }
}

static int lastStripBucket = -1;

static void stripJump(int16_t y) {
  const JumpTable *j = levelJump(level());
  if (!j) {
    return;
  }
  int b = constrain((y - LIST_Y - STRIP_TOP) / STRIP_PITCH, 0, 26);
  if (b == lastStripBucket) {
    return;
  }
  lastStripBucket = b;
  while (b < 26 && j->start[b] == j->start[b + 1]) {
    b++;  // empty letter: go to the next one that has entries
  }
  scrollTo(j->start[b]);
}

static void pushLevel(ListKind kind, uint32_t param, const String &title) {
  Tab &tab = tabs[activeTab];
  if (tab.depth >= MAX_NAV_DEPTH) {
    return;
  }
  Level &l = tab.stack[tab.depth++];
  l = Level();
  l.kind = kind;
  l.param = param;
  l.title = title;
  drawHeader();
  drawList();
}

static void goBack() {
  Tab &tab = tabs[activeTab];
  if (tab.depth > 1) {
    tab.depth--;
    drawHeader();
    drawList();
  }
}

static void selectTab(int t) {
  if (t == activeTab) {
    if (tabs[t].depth == 1) {
      return;
    }
    tabs[t].depth = 1;  // tapping the current tab goes back to its top
  }
  activeTab = t;
  releaseThumbs();  // forget "missing": pictures may have arrived meanwhile
  drawTabs();
  drawHeader();
  drawList();
}

// A tap on a Settings row changes it.
static void logSetting(uint32_t id) {
  Serial.printf("[ui] %s: %s\n", settingLabel(id), settingValue(id).c_str());
}

// A choice picked: applied at once; the list stays, so others can be tried.
static void pickChoice(uint32_t id, uint8_t i) {
  UiStyle style = uiStyle;
  switch (id) {
    case S_THEME: style.theme = i; break;
    case S_DENSITY: style.compact = i; break;
    case S_TEXT: style.large = i; break;
    case S_CORNERS: style.square = i; break;
    case S_BRIGHTNESS: style.brightness = i; break;
    case S_SLEEP: style.sleep = i; break;
    case S_ART: {
      CardSettings cs;
      cardSettingsLoad(cs);
      cs.artDownload = i;
      cardSettingsSave(cs);
      artFetchSetEnabled(cs.artDownload);
      if (!cs.artDownload) artFetchStop();
      logSetting(id);
      drawHeader();
      drawRows();
      return;
    }
    default: return;
  }
  uiStyleSet(style);
  logSetting(id);
  if (id == S_BRIGHTNESS || id == S_SLEEP) {
    drawRows();
    return;
  }
  stripStale = true;
  if (style.theme == THEME_COVER) uiThemeCover(currentThumb());
  clearRowCache();
  listMarquee.active = false;
  uiShowPlayer();  // new colours or row heights: redraw everything
}

// A setting tapped: open its choices, or run it.
static void tapSetting(uint32_t id) {
  if (choiceCount(id)) {
    pushLevel(L_SETTING_CHOICE, id, settingLabel(id));
    return;
  }
  switch (id) {
    case S_WIFI: wifiSetupRequested = true; break;
    case S_ART_NOW:
      artFetchNow();
      drawRows();
      break;
    case S_REBUILD:
      if (rebuildArmedAt && millis() - rebuildArmedAt < 4000) {
        rebuildArmedAt = 0;
        rebuildRequested = true;
      } else {
        rebuildArmedAt = millis() | 1;
      }
      drawRows();
      break;
    default: break;  // information
  }
}

// Play all (in order) or Shuffle on an artist or album page.
static void playAll(const Level &l, bool shuffle) {
  PlayContext c;
  ArtistRecord a;
  if (l.kind == L_ARTIST_ALBUMS) {
    if (!Catalog::artist(l.param, a)) return;
    c = {CTX_ARTIST_IDX, a.firstSongPos, a.songCount};
  } else {
    c = levelContext(l);
  }
  if (!c.count) return;
  shuffleOn = shuffle;
  savePlayModes();
  playCtx = c;
  playFromTitle = l.title;
  playIndex = shuffle ? esp_random() % c.count : 0;
  reshuffle(playIndex, c.count);
  startPlayback();
}

static void tapRow(int r) {
  const Level l = level();  // copy: pushLevel changes level()
  const int32_t index = l.top + r;
  Row row;
  if (index >= levelCount(l) || !levelRow(l, index, row)) {
    return;
  }
  switch (row.kind) {
    case R_SONG: playFromLevel(l, index - songRowOffset(l)); break;
    case R_ARTIST: pushLevel(L_ARTIST_ALBUMS, row.id, row.line1); break;
    case R_ALL_SONGS: pushLevel(L_ARTIST_SONGS, row.id, "All songs"); break;
    case R_ALBUM: pushLevel(L_ALBUM_SONGS, row.id, row.line1); break;
    case R_FOLDER: pushLevel(L_FOLDER, row.id, row.line1); break;
    case R_SETTING: tapSetting(row.id); break;
    case R_GROUP: pushLevel(L_SETTINGS_GROUP, row.id, kGroupNames[row.id]); break;
    case R_CHOICE: pickChoice(l.param, row.id); break;
    case R_ACTIONS: playAll(l, pressX >= 10 + (LIST_W - 2 * 10 - ACTIONS_GAP) / 2 + ACTIONS_GAP / 2); break;
    default: break;
  }
}

static bool stripDrag = false;
static uint32_t statusHoldStart = 0;  // nonzero while the header's status is held
#define REBUILD_HOLD_MS 2000

bool uiTakeRebuildRequest() {
  const bool requested = rebuildRequested;
  rebuildRequested = false;
  return requested;
}

bool uiTakeWifiSetupRequest() {
  const bool requested = wifiSetupRequested;
  wifiSetupRequested = false;
  return requested;
}

// ---------------------------------------------------------------- Search
// The header's search icon (Songs, Artists, Albums) opens a field with the
// keyboard; "Search" finds the text anywhere in artist and album names and
// in songs' titles, artists and albums, and shows the hits as a list
// (artists, albums, then songs A-Z; songs play in that order).
#define SEARCH_KB_Y (SCREEN_H - KB_H)
#define SEARCH_MAX_HITS 200
static bool searchOpen = false;
static String searchQuery;

struct SearchDraw {  // unmutes the LCD for the search screen's drawing
  SearchDraw() { lcdSetMuted(false); }
  ~SearchDraw() { lcdSetMuted(searchOpen || nowOpen); }
};

static void drawSearchField() {
  canvas->fillRect(0, 0, SCREEN_W, HEADER_H, pal.bg);
  uiDrawIcon(ICONS_24, ICON_BACK, 4, 4, pal.text);
  const int16_t fx = 32, fw = SCREEN_W - 40;
  canvas->fillRect(fx, 4, fw, 24, pal.surface);
  roundRect(fx, 4, fw, 24, uiRadiusPill(24), pal.bg);
  uiDrawIcon(ICONS_18, ICON_SEARCH, fx + 6, 7, pal.faint);
  if (searchQuery.isEmpty()) {
    uiDrawText(FONT_SMALL, "Songs, artists, albums", fx + 28, 20, pal.faint);
  } else {
    String shown = searchQuery + "_";
    while (shown.length() > 1 && uiTextWidth(FONT_BODY, shown.c_str()) > fw - 36) shown.remove(0, 1);
    uiDrawText(FONT_BODY, shown.c_str(), fx + 28, 21, pal.text);
  }
  lcdBlit(0, 0, SCREEN_W, HEADER_H);
}

static void drawSearchHint(const String &a, const String &b) {
  lcdFillRect(0, HEADER_H, SCREEN_W, SEARCH_KB_Y - HEADER_H, pal.bg);
  canvas->fillRect(0, 0, SCREEN_W, 40, pal.bg);
  drawCentered(FONT_BODY, uiFitText(FONT_BODY, a, SCREEN_W - 20), SCREEN_W / 2, 16, pal.dim);
  drawCentered(FONT_SMALL, uiFitText(FONT_SMALL, b, SCREEN_W - 20), SCREEN_W / 2, 33, pal.faint);
  lcdBlit(0, HEADER_H + 40, SCREEN_W, 40);
}

static void openSearch() {
  stopList();
  listMarquee.active = false;
  searchOpen = true;
  searchQuery = "";
  SearchDraw unmute;
  setListOffset(0);  // this screen covers the hardware-scrolled list area
  drawSearchField();
  drawSearchHint("Search your library", "Type part of a name, then Search");
  keyboardShow(SEARCH_KB_Y, "Search");
}

static void dropSearch() {
  if (searchOpen) {
    searchOpen = false;
    lcdSetMuted(nowOpen);
    lcdScrollOffset(listOffset);
  }
}

static bool containsNoCase(const char *hay, const String &needle) {
  const size_t n = needle.length();
  for (; *hay; hay++) {
    if (strncasecmp(hay, needle.c_str(), n) == 0) return true;
  }
  return false;
}

// Runs the search; true if anything was found (searchHits filled).
static bool runSearch(const String &q) {
  searchHits.clear();
  searchSongStart = 0;
  const uint32_t t0 = millis();
  ArtistRecord a;
  for (uint32_t i = 0; i < Catalog::artistCount() && searchHits.size() < SEARCH_MAX_HITS; i++) {
    if (Catalog::artist(i, a) && containsNoCase(a.name, q)) searchHits.push_back({R_ARTIST, i});
  }
  AlbumRecord al;
  uint32_t id;
  for (uint32_t p = 0; p < Catalog::albumIndexCount() && searchHits.size() < SEARCH_MAX_HITS; p++) {
    if (Catalog::albumByName(p, id) && Catalog::album(id, al) && !(al.flags & ALBUM_NONE_ALL) &&
        containsNoCase(al.name, q)) {
      searchHits.push_back({R_ALBUM, id});
    }
  }
  searchSongStart = searchHits.size();
  // Songs: one pass over songs.dat. Copies of a song (hidden from the lists)
  // are told apart only by their folder, so equal title + artist is shown once.
  struct Found {
    uint32_t id, same;
    String title;
  };
  std::vector<Found> found;
  LibReader songs;
  if (songs.open(Catalog::generation(), LF_SONGS, sizeof(SongRecord))) {
    songs.setReadAhead(8192);
    SongRecord r;
    for (uint32_t i = 0; i < songs.count() && searchHits.size() + found.size() < SEARCH_MAX_HITS; i++) {
      if (!songs.read(i, &r)) break;
      if (!containsNoCase(r.title, q) && !containsNoCase(r.artist, q) && !containsNoCase(r.album, q)) continue;
      String key = String(r.title) + "\x1f" + r.artist;
      key.toLowerCase();
      uint32_t h = 2166136261u;
      for (size_t k = 0; k < key.length(); k++) h = (h ^ static_cast<uint8_t>(key[k])) * 16777619u;
      bool dup = false;
      for (const Found &f : found) dup = dup || f.same == h;
      if (!dup) found.push_back({i, h, String(r.title)});
    }
  }
  std::sort(found.begin(), found.end(),
            [](const Found &x, const Found &y) { return strcasecmp(x.title.c_str(), y.title.c_str()) < 0; });
  for (const Found &f : found) searchHits.push_back({R_SONG, f.id});
  Serial.printf("[search] \"%s\": %u artists and albums, %u songs in %lu ms\n", q.c_str(), searchSongStart,
                found.size(), millis() - t0);
  return !searchHits.empty();
}

static void searchTouch(TouchPhase phase, int16_t x, int16_t y) {
  SearchDraw unmute;
  if (phase == TOUCH_DOWN && y < HEADER_H && x < 36) {
    dropSearch();
    uiShowPlayer();
    return;
  }
  if (y < SEARCH_KB_Y && phase == TOUCH_DOWN) return;
  const KeyPress k = keyboardTouch(phase, x, y);
  switch (k.action) {
    case KEY_CHAR:
      if (searchQuery.length() < 40) searchQuery += k.c;
      drawSearchField();
      break;
    case KEY_DELETE:
      if (searchQuery.length()) searchQuery.remove(searchQuery.length() - 1);
      drawSearchField();
      break;
    case KEY_BACK:
      dropSearch();
      uiShowPlayer();
      break;
    case KEY_ENTER: {
      String q = searchQuery;
      q.trim();
      if (q.length() < 2) {
        drawSearchHint("Type at least 2 letters", "");
        break;
      }
      drawSearchHint("Searching...", q);
      if (!runSearch(q)) {
        drawSearchHint("Nothing found", q);
        break;
      }
      dropSearch();
      Tab &tab = tabs[activeTab];
      if (tab.depth < MAX_NAV_DEPTH) {
        Level &l = tab.stack[tab.depth++];
        l = Level();
        l.kind = L_SEARCH;
        l.title = "\xE2\x80\x9C" + q + "\xE2\x80\x9D";
      }
      clearRowCache();
      uiShowPlayer();
      break;
    }
    default: break;
  }
}

static void nowTouch(TouchPhase phase, int16_t x, int16_t y) {
  if (phase == TOUCH_UP) {
    volumeDrag = false;
    return;
  }
  if (phase == TOUCH_MOVE) {
    if (volumeDrag) setVolumeAt(x);
    return;
  }
  if (y < NP_HEADER_H + 6) {
    if (x < 48) {
      closeNow();
    } else if (x >= SCREEN_W - 48) {
      nowLyrics = !nowLyrics;
      drawNow();
    }
  } else if (y >= NP_BAR_Y && y < NP_BAR_Y + NP_BAR_H) {  // seek bar
    if (current.durationMs) {
      seekTo(static_cast<uint64_t>(constrain(x - NP_BAR_X, 0, NP_BAR_W)) * current.durationMs / NP_BAR_W);
      NowDraw unmute;
      drawNowBar();
    }
  } else if (nowLyrics && y < NP_BAR_Y && lyricSynced) {  // a lyric line
    const int i = lyricFirst + (y - LYRICS_Y) / LYRIC_H;
    if (i >= 0 && i < lyricCount) {
      seekTo(lyricLines[i].ms);
      NowDraw unmute;
      drawLyrics(true);
    }
  } else if (y >= NP_CTRL_Y && y < NP_CTRL_Y + NP_CTRL_H) {
    if (x < 40) toggleShuffle();
    else if (x < 92) previousTrack();
    else if (x < 148) togglePlayPause();
    else if (x < 200) nextTrack();
    else cycleRepeat();
    NowDraw unmute;
    drawNowControls();
  } else if (y >= NP_VOL_Y - 4) {
    volumeDrag = true;
    setVolumeAt(x);
  }
}

// Screen off after the chosen time without a touch; the touch that wakes
// it does nothing else.
static uint32_t lastTouchAt = 0;
static bool screenAsleep = false, swallowTouch = false;

void uiHandleTouch(TouchPhase phase, int16_t x, int16_t y) {
  lastTouchAt = millis();
  if (screenAsleep) {
    if (phase == TOUCH_DOWN) {
      screenAsleep = false;
      lcdSetBacklight(uiBrightnessPercent(uiStyle.brightness));
      Serial.println("[ui] screen on (the waking touch is ignored)");
      swallowTouch = true;
    }
    return;
  }
  if (swallowTouch) {
    if (phase == TOUCH_UP) swallowTouch = false;
    return;
  }
  if (searchOpen) {
    searchTouch(phase, x, y);
    return;
  }
  if (nowOpen) {
    nowTouch(phase, x, y);
    return;
  }
  if (phase == TOUCH_UP) {
    if (motion == M_PRESS || motion == M_DRAG) {
      listTouchUp();
    }
    stripDrag = false;
    lastStripBucket = -1;
    if (statusHoldStart) {
      statusHoldStart = 0;
      uiSetLibraryStatus("");  // released early: clear the "Hold" hint
    }
    return;
  }
  if (phase == TOUCH_MOVE) {
    if (stripDrag) {
      stripJump(y);
    } else if (motion == M_PRESS || motion == M_DRAG) {
      listTouchMove(y);
    }
    return;
  }
  // TOUCH_DOWN
  if (y >= LIST_Y && y < LIST_END && x < LIST_W) {
    listTouchDown(x, y);  // a tap fires on release if the finger didn't move
    return;
  }
  stopList();  // anything else first ends list movement
  if (y < HEADER_H) {
    if (x < 40 && tabs[activeTab].depth > 1) {
      goBack();
    } else if (searchIconX >= 0 && x >= searchIconX - 6 && x < searchIconX + 24) {
      openSearch();
    } else if (x >= SCREEN_W - 60 && libraryStatus.isEmpty()) {  // hold the status to rebuild
      statusHoldStart = millis() | 1;
      uiSetLibraryStatus("Hold to rebuild");
    }
    return;
  }
  if (y < LIST_END) {
    if (levelJump(level())) {  // letter strip (list touches were handled above)
      stripDrag = true;
      stripJump(y);
    }
    return;
  }
  if (y < TABBAR_Y) {  // mini player
    if (x >= MINI_PLAY_X && x < MINI_NEXT_X) {
      togglePlayPause();
    } else if (x >= MINI_NEXT_X) {
      nextTrack();
    } else if (playIndex >= 0) {
      openNow();
    }
    return;
  }
  selectTab(min<int>(x / TAB_W, TAB_COUNT - 1));
}

void uiSetLibraryStatus(const String &text) {
  if (text == libraryStatus) {
    return;
  }
  libraryStatus = text;
  if (!nowOpen) drawHeader();
}

void uiLibraryChanged() {
  // Ids changed. Find the playing song again by its path; it continues in
  // the "all songs" order. Navigation goes back to each tab's top.
  if (playIndex >= 0) {
    uint32_t pos;
    if (Catalog::findTitlePos(currentPath, current.title, pos)) {
      playCtx = {CTX_TITLE, 0, Catalog::titleCount()};
      playIndex = pos;
      reshuffle(pos, playCtx.count);
      uint32_t id;
      if (Catalog::titleSong(pos, id)) {
        Catalog::song(id, current);
      }
    } else {
      playIndex = -1;  // removed from the card; it plays on until it ends
      playCtx = PlayContext();
    }
  }
  const int32_t songsTop = tabs[0].stack[0].top;
  resetTabs();
  tabs[0].stack[0].top = min<int32_t>(songsTop, max<int32_t>(0, Catalog::songCount() - 1));
  listMarquee.active = false;
  clearRowCache();
  uiShowPlayer();
}

void uiTick() {
  const uint32_t sleepMs = uiSleepMs(uiStyle.sleep);
  if (!lastTouchAt) lastTouchAt = millis();
  if (sleepMs && !screenAsleep && millis() - lastTouchAt > sleepMs) {
    screenAsleep = true;
    lcdSetBacklight(0);
    Serial.println("[ui] screen off");
  }
  if (audioTakeTrackEnded()) {
    handleTrackEnded();
  }
  if (statusHoldStart && millis() - statusHoldStart >= REBUILD_HOLD_MS) {
    statusHoldStart = 0;
    rebuildRequested = true;
  }
  if (rebuildArmedAt && millis() - rebuildArmedAt >= 4000) {  // "Tap again" expired
    rebuildArmedAt = 0;
    if (!nowOpen && level().kind == L_SETTINGS_GROUP && !listMoving()) drawRows();
  }

  // "Cover colour" theme: follow the playing album.
  static uint32_t themedSong = LIB_NONE;
  const uint32_t song = playIndex >= 0 ? current.id : LIB_NONE;
  if (uiStyle.theme == THEME_COVER && song != themedSong && !listMoving()) {
    themedSong = song;
    if (uiThemeCover(currentThumb())) {
      clearRowCache();
      stripStale = true;
      if (nowOpen) drawNow();
      else uiShowPlayer();
    }
  }

  if (nowOpen && playIndex >= 0 && current.id != nowSongId) {
    drawNow();  // the next song started
  }

  tickList();
  if (!listMoving() && !nowOpen) {
    marqueeTick(listMarquee);
  }
  if (nowOpen) {
    NowDraw unmute;
    marqueeTick(titleMarquee);
  } else if (lyricCount && current.id != lyricSongId) {
    lyricsFree();  // another song: load its lyrics when Now Playing opens
    lyricSongId = LIB_NONE;
  }

  static uint32_t lastSlow = 0;
  if (millis() - lastSlow > 500) {
    lastSlow = millis();
    if (audioState() == PLAYING) {
      if (nowOpen) {
        NowDraw unmute;
        if (nowLyrics) drawLyrics(false);
        drawNowBar();
      } else {
        drawMiniProgress(false);
      }
    }
    if (!nowOpen && wifiState() != wifiShown && !listMoving()) drawHeader();
  }
}
