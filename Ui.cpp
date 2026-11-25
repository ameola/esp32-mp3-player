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
};

enum RowKind : uint8_t { R_NONE, R_SONG, R_ARTIST, R_ALBUM, R_ALL_SONGS, R_FOLDER };

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
  static const char *const labels[TAB_COUNT] = {"Songs", "Artists", "Albums", "Folders"};
  static const ListKind roots[TAB_COUNT] = {L_SONGS, L_ARTISTS, L_ALBUMS, L_FOLDER};
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

static bool ctxSong(const PlayContext &c, uint32_t i, uint32_t &id) {
  if (i >= c.count) {
    return false;
  }
  switch (c.kind) {
    case CTX_TITLE: return Catalog::titleSong(c.start + i, id);
    case CTX_ARTIST_IDX: return Catalog::artistSong(c.start + i, id);
    case CTX_SONG_IDS: id = c.start + i; return true;
    case CTX_NONE_IDX: return Catalog::noneSong(c.start + i, id);
    default: return false;
  }
}

static bool isPlaying(uint32_t songId) {
  return playIndex >= 0 && current.id == songId;
}

// ---------------------------------------------------------------- Level contents
static PlayContext levelContext(const Level &l) {
  PlayContext c;
  ArtistRecord a;
  AlbumRecord al;
  FolderRecord f;
  switch (l.kind) {
    case L_SONGS:
      c = {CTX_TITLE, 0, Catalog::songCount()};
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
    default:
      break;
  }
  return c;
}

// Rows before the first song row (a folder lists its subfolders first).
static uint32_t songRowOffset(const Level &l) {
  FolderRecord f;
  return (l.kind == L_FOLDER && Catalog::folder(l.param, f)) ? f.childCount : 0;
}

static int32_t levelCount(const Level &l) {
  ArtistRecord a;
  AlbumRecord al;
  FolderRecord f;
  switch (l.kind) {
    case L_SONGS: return Catalog::songCount();
    case L_ARTISTS: return Catalog::artistCount();
    case L_ARTIST_ALBUMS: return Catalog::artist(l.param, a) ? a.albumCount + 1 : 0;
    case L_ARTIST_SONGS: return Catalog::artist(l.param, a) ? a.songCount : 0;
    case L_ALBUMS: return Catalog::albumCount();
    case L_ALBUM_SONGS: return Catalog::album(l.param, al) ? al.songCount : 0;
    case L_FOLDER: return Catalog::folder(l.param, f) ? f.childCount + f.songCount : 0;
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
      if (i == 0) {
        row.kind = R_ALL_SONGS;
        row.id = l.param;
        row.line1 = "All songs";
        row.line2 = plural(a.songCount, "song");
        row.art = artArtistKey(a.name);
        return true;
      }
      if (!Catalog::album(a.firstAlbum + i - 1, al)) return false;
      row.kind = R_ALBUM;
      row.id = a.firstAlbum + i - 1;
      row.line1 = al.name;
      row.line2 = (al.year ? String(al.year) + ", " : String("")) + plural(al.songCount, "song");
      if (!(al.flags & ALBUM_NONE)) row.art = artAlbumKey(a.name, al.name);
      return true;
    case L_ALBUMS:
      if (!Catalog::albumByName(i, id) || !Catalog::album(id, al)) return false;
      row.kind = R_ALBUM;
      row.id = id;
      row.line1 = al.name;
      if (al.flags & ALBUM_NONE_ALL) {  // every artist's loose songs
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
    default: {  // song lists
      uint32_t songId;
      // Track numbers in an album, not in a "none" album (by title).
      const bool tracks = l.kind == L_ALBUM_SONGS && Catalog::album(l.param, al) &&
                          !(al.flags & (ALBUM_NONE | ALBUM_NONE_ALL));
      return ctxSong(levelContext(l), i, songId) && songRow(songId, tracks, row);
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
  uint8_t size = 1;
  uint16_t fg = 0, bg = 0;
  int16_t textW = 0;
  int16_t offset = 0;
  uint32_t nextStep = 0;
};

#define MARQUEE_GAP 40       // pixels between the end of the text and its repeat
#define MARQUEE_STEP_PX 3
#define MARQUEE_STEP_MS 100
#define MARQUEE_PAUSE_MS 2000

static Marquee listMarquee;    // playing row's title in the list
static Marquee titleMarquee;   // now-playing title
static Marquee artistMarquee;  // now-playing artist

static void marqueeSet(Marquee &m, const String &text, int16_t x, int16_t y, int16_t w,
                       uint8_t size, uint16_t fg, uint16_t bg) {
  m.text = text;
  m.x = x;
  m.y = y;
  m.w = w;
  m.h = 8 * size;
  m.size = size;
  m.fg = fg;
  m.bg = bg;
  m.textW = text.length() * 6 * size;
  m.offset = 0;
  m.nextStep = millis() + MARQUEE_PAUSE_MS;
  m.active = m.textW > w;
}

// Draws the strip into the canvas with its top-left at (ox, oy), clipped to
// the strip width.
static void marqueeCompose(const Marquee &m, int16_t ox, int16_t oy) {
  canvas->fillRect(ox, oy, m.w, m.h, m.bg);
  canvas->setTextSize(m.size);
  canvas->setTextColor(m.fg);
  canvas->setCursor(ox - m.offset, oy);
  canvas->print(m.text);
  if (m.offset > 0) {
    canvas->setCursor(ox - m.offset + m.textW + MARQUEE_GAP, oy);
    canvas->print(m.text);
    canvas->fillRect(0, oy, ox, m.h, m.bg);  // clip the left edge
  }
  canvas->fillRect(ox + m.w, oy, canvas->width() - ox - m.w, m.h, m.bg);
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
    listBlitMarquee(m);
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

static void dropCover();

// ================================================================ Drawing
// Each draw function composes its region at (0,0) of the canvas, then blits.
static String libraryStatus;
static int lastProgressPx = -1;
static int32_t lastTimeShown = -1;  // elapsed seconds currently on screen

#define TIME_W 102  // "1:02:03 / 1:10:00" at text size 1
#define TIME_X (SCREEN_W - 8 - TIME_W)
#define STATUS_W (SCREEN_W - STATUS_X)

static void drawCenteredText(const String &text, int16_t cx, int16_t y, uint8_t size,
                             uint16_t color) {
  canvas->setTextSize(size);
  canvas->setTextColor(color);
  canvas->setCursor(cx - static_cast<int16_t>(text.length() * 6 * size) / 2, y);
  canvas->print(text);
}

static void composeLibraryStatus(int16_t ox) {
  canvas->fillRect(ox, 0, STATUS_W, TAB_H, C_HEADER);
  const int nl = libraryStatus.indexOf('\n');
  const String a = nl < 0 ? libraryStatus : libraryStatus.substring(0, nl);
  const String b = nl < 0 ? String("") : libraryStatus.substring(nl + 1);
  drawCenteredText(fitText(a, 8), ox + STATUS_W / 2, b.isEmpty() ? 8 : 3, 1, C_ACCENT);
  drawCenteredText(fitText(b, 8), ox + STATUS_W / 2, 13, 1, C_ACCENT);
}

static void drawTabs() {
  canvas->fillRect(0, 0, SCREEN_W, TAB_H, C_HEADER);
  for (int t = 0; t < TAB_COUNT; t++) {
    const bool active = t == activeTab;
    if (active) {
      canvas->fillRect(t * TAB_W, 0, TAB_W, TAB_H, C_TAB_ACTIVE);
      canvas->fillRect(t * TAB_W, TAB_H - 3, TAB_W, 3, C_ACCENT);
    }
    drawCenteredText(tabs[t].label, t * TAB_W + TAB_W / 2, 8, 1, active ? C_TEXT : C_DIM);
  }
  composeLibraryStatus(STATUS_X);
  lcdBlit(0, 0, SCREEN_W, TAB_H);
}

// Back button, trail ("Artists > Smash Mouth > Astro Lounge", trimmed from
// the left to fit) and volume.
static void drawNav() {
  canvas->fillRect(0, 0, SCREEN_W, NAV_H, C_BG);
  const Tab &tab = tabs[activeTab];
  int16_t textX = 8;
  if (tab.depth > 1) {
    canvas->fillRoundRect(2, 2, BACK_W - 4, NAV_H - 4, 3, C_BUTTON);
    canvas->fillTriangle(8, NAV_H / 2, 16, 5, 16, NAV_H - 6, C_TEXT);
    textX = BACK_W + 4;
  }
  String trail;
  for (int i = 0; i < tab.depth; i++) {
    trail += (i ? " > " : "") + tab.stack[i].title;
  }
  const size_t maxChars = (VOL_DOWN_X - 4 - textX) / 6;
  if (trail.length() > maxChars) {
    trail = "..." + trail.substring(trail.length() - (maxChars - 3));
  }
  canvas->setTextSize(1);
  canvas->setTextColor(C_TEXT);
  canvas->setCursor(textX, 7);
  canvas->print(trail);

  canvas->fillRoundRect(VOL_DOWN_X, 2, VOL_BTN_W - 2, NAV_H - 4, 3, C_BUTTON);
  canvas->fillRect(VOL_DOWN_X + 7, NAV_H / 2 - 1, 10, 2, C_TEXT);
  canvas->fillRoundRect(VOL_UP_X, 2, VOL_BTN_W - 2, NAV_H - 4, 3, C_BUTTON);
  canvas->fillRect(VOL_UP_X + 7, NAV_H / 2 - 1, 10, 2, C_TEXT);
  canvas->fillRect(VOL_UP_X + 11, NAV_H / 2 - 5, 2, 10, C_TEXT);
  drawCenteredText(String(audioVolume()), VOL_TEXT_X + (VOL_UP_X - VOL_TEXT_X) / 2, 7, 1,
                   C_ACCENT);
  lcdBlit(0, NAV_Y, SCREEN_W, NAV_H);
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

// Composes list row `index` (data `row`, or nullptr for an empty row) into the
// canvas at (0,0). Stripes follow the index so they move with the list. When
// marqueeY >= 0 the row is at rest at that screen line and a playing row's
// title scrolls; while the list moves it's drawn trimmed instead.
static void composeRow(int32_t index, const Row *row, int16_t marqueeY) {
  const bool playing = row && row->kind == R_SONG && isPlaying(row->id);
  const uint16_t bg = playing ? C_ROW_PLAYING : (index % 2 ? C_ROW_ALT : C_BG);
  canvas->fillRect(0, 0, LIST_W, ROW_H, bg);
  if (!row) {
    return;
  }
  const bool drill = row->kind != R_SONG;
  // Album and song rows have a cover slot; folder rows (which share lists
  // with songs) a big folder icon in the same place, so the text lines up.
  const bool person = row->kind == R_ARTIST || row->kind == R_ALL_SONGS;
  const bool thumb = person || row->kind == R_ALBUM || row->kind == R_SONG;
  const int16_t textX = thumb || row->kind == R_FOLDER ? 36 : 18;
  const int16_t textW = LIST_W - textX - (drill ? 14 : 4);
  if (thumb) {
    const uint16_t *px = nullptr;
    bool missing = true;
    if (row->art) px = thumbFor(row->art, marqueeY >= 0, &missing);
    if (!px && missing && row->art2) px = thumbFor(row->art2, marqueeY >= 0, &missing);
    if (px) {
      canvas->drawRGBBitmap(1, 1, px, ART_THUMB, ART_THUMB);
    } else if (person) {  // placeholder: head and shoulders
      canvas->fillRect(1, 1, ART_THUMB, ART_THUMB, C_HEADER);
      canvas->fillCircle(16, 12, 5, C_DIM);
      canvas->fillRoundRect(7, 20, 18, 11, 5, C_DIM);
      canvas->fillRect(7, 27, 18, 4, C_HEADER);
    } else {  // placeholder: a dim square with a note
      canvas->fillRect(1, 1, ART_THUMB, ART_THUMB, C_HEADER);
      canvas->fillCircle(13, 21, 3, C_DIM);
      canvas->fillRect(15, 9, 2, 12, C_DIM);
      canvas->fillRect(15, 9, 6, 2, C_DIM);
    }
  }
  if (playing) {  // a play badge over the cover
    canvas->fillCircle(16, 16, 9, C_BG);
    canvas->fillTriangle(13, 10, 13, 22, 22, 16, C_ACCENT);
  } else if (row->kind == R_FOLDER) {  // folder icon
    canvas->fillRect(4, 7, 10, 3, C_DIM);
    canvas->fillRect(4, 10, 24, 15, C_DIM);
  }
  if (playing && marqueeY >= 0) {
    marqueeSet(listMarquee, row->line1, textX, marqueeY, textW, 2, C_ACCENT, bg);
    marqueeCompose(listMarquee, textX, 4);
  } else {
    canvas->setTextSize(2);
    canvas->setTextColor(playing || row->kind == R_ALL_SONGS ? C_ACCENT : C_TEXT);
    canvas->setCursor(textX, 4);
    canvas->print(fitText(row->line1, textW / 12));
  }
  if (drill) {  // chevron: tapping opens it
    canvas->fillTriangle(LIST_W - 11, 10, LIST_W - 11, 20, LIST_W - 6, 15, C_DIM);
  }
  canvas->setTextSize(1);
  canvas->setTextColor(C_DIM);
  const size_t rightChars = row->right.length();
  canvas->setCursor(textX, 22);
  canvas->print(fitText(row->line2, (LIST_W - textX - (drill ? 14 : 4)) / 6 - (rightChars ? rightChars + 1 : 0)));
  if (rightChars) {
    canvas->setCursor(LIST_W - 4 - rightChars * 6, 22);
    canvas->print(row->right);
  }
}

static void drawRow(int r) {
  const Level &l = level();
  const int32_t index = l.top + r;
  const int16_t rowY = LIST_Y + r * ROW_H;
  const int32_t count = levelCount(l);
  Row row;
  const bool have = index < count && levelRow(l, index, row);
  const bool playing = have && row.kind == R_SONG && isPlaying(row.id);
  if (!playing && listMarquee.y == rowY + 4) {
    listMarquee.active = false;  // this row no longer shows the playing song
  }
  composeRow(index, have ? &row : nullptr, rowY + 4);
  if (!have && r == 0 && count == 0) {
    canvas->setTextSize(1);
    canvas->setTextColor(C_DIM);
    canvas->setCursor(18, 12);
    canvas->print(Catalog::songCount() ? "Nothing here" : "No MP3 files found");
  }
  listBlit(0, rowY, LIST_W, ROW_H);
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

static void drawStrip() {
  const Level &l = level();
  const JumpTable *j = levelJump(l);
  const int current = stripLetter(l, j);
  stripShown = current;
  const int perChunk = 6;  // 6 letters x 7 px = 42 px, fits the canvas
  for (int first = 0; first < 27; first += perChunk) {
    const int n = min(perChunk, 27 - first);
    canvas->fillRect(0, 0, STRIP_W, n * STRIP_PITCH, C_HEADER);
    if (j) {
      for (int k = 0; k < n; k++) {
        const int b = first + k;
        const char c = b == 0 ? '#' : static_cast<char>('A' + b - 1);
        const uint16_t color = b == current ? C_ACCENT
                               : j->start[b] < j->start[b + 1] ? C_TEXT : C_BUTTON_PRESS;
        canvas->drawChar(7, k * STRIP_PITCH, c, color, color, 1);
      }
    }
    listBlit(LIST_W, LIST_Y + first * STRIP_PITCH, STRIP_W, n * STRIP_PITCH);
  }
  listFill(LIST_W, LIST_Y + 27 * STRIP_PITCH, STRIP_W, LIST_END - LIST_Y - 27 * STRIP_PITCH,
           C_HEADER);
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
  scrollMaxPx = max<int32_t>(max<int32_t>(0, scrollCount - VISIBLE_ROWS) * ROW_H, scrollPx);
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

static void listTouchDown(int16_t y) {
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

static void drawProgress(bool force) {
  int px = 0;
  const uint32_t size = audioFileSize();
  if (playIndex >= 0 && size > 0) {
    px = static_cast<int>(static_cast<uint64_t>(audioFilePos()) * PROGRESS_W / size);
  }
  px = constrain(px, 0, PROGRESS_W);
  if (!force && px == lastProgressPx) {
    return;
  }
  if (force || px < lastProgressPx) {
    lcdFillRect(PROGRESS_X + px, PROGRESS_Y, PROGRESS_W - px, PROGRESS_H, C_PROGRESS_BG);
    lcdFillRect(PROGRESS_X, PROGRESS_Y, px, PROGRESS_H, C_ACCENT);
  } else {
    lcdFillRect(PROGRESS_X + lastProgressPx, PROGRESS_Y, px - lastProgressPx,
                PROGRESS_H, C_ACCENT);
  }
  lastProgressPx = px;
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

// Composes "elapsed / total" right-aligned into the canvas at (ox, oy).
static void composeTime(int16_t ox, int16_t oy) {
  canvas->fillRect(ox, oy, TIME_W, 8, C_BG);
  if (playIndex < 0) {
    return;
  }
  String text = formatTime(elapsedMs());
  if (current.durationMs > 0) {
    text += " / " + formatTime(current.durationMs);
  }
  canvas->setTextSize(1);
  canvas->setTextColor(C_DIM);
  canvas->setCursor(ox + TIME_W - text.length() * 6, oy);
  canvas->print(text);
}

static void drawTime(bool force) {
  const int32_t seconds = elapsedMs() / 1000;
  if (!force && seconds == lastTimeShown) {
    return;
  }
  lastTimeShown = seconds;
  composeTime(0, 0);
  lcdBlit(TIME_X, NOW_Y + 4, TIME_W, 8);
}

static void drawNowPlaying() {
  canvas->fillRect(0, 0, SCREEN_W, NOW_H, C_BG);
  canvas->drawFastHLine(0, 0, SCREEN_W, C_HEADER);
  canvas->setTextSize(1);
  canvas->setCursor(8, 4);
  if (playIndex < 0) {
    titleMarquee.active = false;
    artistMarquee.active = false;
    canvas->setTextColor(C_DIM);
    canvas->print(Catalog::songCount() == 0 ? "No MP3 files found" : "Tap a song to play");
  } else {
    const PlayerState state = audioState();
    canvas->setTextColor(C_DIM);
    canvas->print(state == PLAYING ? "Playing " : state == PAUSED ? "Paused " : "Stopped ");
    canvas->printf("%ld/%lu", playIndex + 1, playCtx.count);
    // Keep the scroll position when only the play state changed.
    const String artist = current.artist.isEmpty() ? "Unknown artist" : current.artist;
    if (titleMarquee.text != current.title || titleMarquee.y != NOW_Y + 14) {
      marqueeSet(titleMarquee, current.title, 8, NOW_Y + 14, SCREEN_W - 16, 1, C_TEXT, C_BG);
    }
    if (artistMarquee.text != artist || artistMarquee.y != NOW_Y + 24) {
      marqueeSet(artistMarquee, artist, 8, NOW_Y + 24, SCREEN_W - 16, 1, C_ACCENT, C_BG);
    }
    marqueeCompose(titleMarquee, 8, 14);
    marqueeCompose(artistMarquee, 8, 24);
    composeTime(TIME_X, 4);
    lastTimeShown = elapsedMs() / 1000;
  }
  lcdBlit(0, NOW_Y, SCREEN_W, NOW_H);
  drawProgress(true);
}

static void drawControlIcon(int button, bool pressed) {
  const int16_t cx = CTRL_W / 2;
  const int16_t cy = CTRL_H / 2;
  canvas->fillRect(0, 0, CTRL_W, CTRL_H, C_BG);
  canvas->fillRoundRect(2, 2, CTRL_W - 4, CTRL_H - 4, 6,
                        pressed ? C_BUTTON_PRESS : C_BUTTON);
  switch (button) {
    case 0:  // page up
      canvas->fillTriangle(cx - 9, cy + 5, cx + 9, cy + 5, cx, cy - 7, C_TEXT);
      break;
    case 1:  // previous
      canvas->fillRect(cx - 9, cy - 8, 3, 16, C_TEXT);
      canvas->fillTriangle(cx + 8, cy - 8, cx + 8, cy + 8, cx - 5, cy, C_TEXT);
      break;
    case 2:  // play / pause
      if (audioState() == PLAYING) {
        canvas->fillRect(cx - 7, cy - 9, 5, 18, C_ACCENT);
        canvas->fillRect(cx + 2, cy - 9, 5, 18, C_ACCENT);
      } else {
        canvas->fillTriangle(cx - 6, cy - 10, cx - 6, cy + 10, cx + 9, cy, C_ACCENT);
      }
      break;
    case 3:  // next
      canvas->fillTriangle(cx - 8, cy - 8, cx - 8, cy + 8, cx + 5, cy, C_TEXT);
      canvas->fillRect(cx + 6, cy - 8, 3, 16, C_TEXT);
      break;
    case 4:  // page down
      canvas->fillTriangle(cx - 9, cy - 5, cx + 9, cy - 5, cx, cy + 7, C_TEXT);
      break;
  }
  lcdBlit(button * CTRL_W, CTRL_Y, CTRL_W, CTRL_H);
}

static void drawControls() {
  for (int b = 0; b < CTRL_BUTTONS; b++) {
    drawControlIcon(b, false);
  }
}

void uiShowStatus(const char *line1, const char *line2) {
  dropCover();
  resetListScroll();
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  drawCenteredText(line1, SCREEN_W / 2, 6, 2, C_TEXT);
  drawCenteredText(line2, SCREEN_W / 2, 30, 1, C_DIM);
  lcdBlit(0, 130, SCREEN_W, CANVAS_H);
}

// Clears the screen under a plain "MP3 Player" header.
static void drawPlainScreen() {
  dropCover();
  resetListScroll();
  lcdFillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H, C_BG);
  canvas->fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
  canvas->setTextSize(2);
  canvas->setTextColor(C_TEXT);
  canvas->setCursor(8, 15);
  canvas->print("MP3 Player");
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
  canvas->drawRect(BAR_X, 13, BAR_W, 12, C_DIM);
  canvas->fillRect(BAR_X + 2, 15, static_cast<int16_t>((BAR_W - 4) * fraction), 8, C_ACCENT);
  drawCenteredText(counts + (counts.isEmpty() ? "" : "   ") + String(static_cast<int>(fraction * 100)) + "%",
                   SCREEN_W / 2, 32, 1, C_DIM);
  lcdBlit(0, PROGRESS_SCREEN_Y, SCREEN_W, CANVAS_H);
  // Folder being worked on: two lines, trimmed from the left.
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  if (folder && folder[0]) {
    const String converted = decodeUtf8(reinterpret_cast<const uint8_t *>(folder), strlen(folder));
    const size_t perLine = (SCREEN_W - 16) / 6;
    String text = converted.length() > perLine * 2
                    ? "..." + converted.substring(converted.length() - (perLine * 2 - 3))
                    : converted;
    canvas->setTextSize(1);
    canvas->setTextColor(C_DIM);
    canvas->setCursor(8, 0);
    canvas->print("Folder:");
    canvas->setTextColor(C_TEXT);
    canvas->setCursor(8, 12);
    canvas->print(text.substring(0, perLine));
    if (text.length() > perLine) {
      canvas->setCursor(8, 24);
      canvas->print(text.substring(perLine));
    }
  }
  lcdBlit(0, FOLDER_Y, SCREEN_W, CANVAS_H);
}

void uiShowPlayer() {
  dropCover();
  static bool initialized = false;
  if (!initialized) {
    resetTabs();
    initialized = true;
  }
  if (!scrollAreaSet) {
    lcdScrollArea(LIST_Y, LIST_H);
    listOffset = 0;
    scrollAreaSet = true;
  }
  motion = M_IDLE;
  drawTabs();
  drawNav();
  drawList();
  drawNowPlaying();
  drawControls();
}

// ================================================================ Cover view
// Tap the now-playing panel: the playing song's album cover, full width,
// with its names and how the cover was drawn (a test view for the art
// cache). Any tap closes it. While it's up the LCD is muted, so the player
// screen's own updates don't draw over it; uiShowPlayer() redraws them.
#define COVER_SIZE SCREEN_W  // 240 x 240 at the top
static bool coverOpen = false;
static uint32_t coverSongId = 0;

// The album a song belongs to: albums with its album name (the library
// keeps 43 bytes of it) whose songs include it. Albums are read in one
// sequential scan, about 140 KB for 2,000 albums.
static bool findAlbumOf(const Track &t, AlbumRecord &album, ArtistRecord &artist) {
  if (t.album.isEmpty()) {
    return false;
  }
  LibReader albums;
  if (!albums.open(Catalog::generation(), LF_ALBUMS, sizeof(AlbumRecord))) {
    return false;
  }
  albums.setReadAhead(4096);
  for (uint32_t i = 0; i < albums.count(); i++) {
    if (!albums.read(i, &album)) {
      return false;
    }
    const String name(album.name, strnlen(album.name, sizeof(album.name)));
    if (name.isEmpty() || !t.album.startsWith(name)) {
      continue;
    }
    for (uint32_t s = 0; s < album.songCount; s++) {
      uint32_t id;
      if (Catalog::artistSong(album.firstSongPos + s, id) && id == t.id) {
        return Catalog::artist(album.artistId, artist);
      }
    }
  }
  return false;
}

static void drawCover() {
  coverSongId = current.id;
  lcdSetMuted(false);
  setListOffset(0);  // the cover covers the hardware-scrolled list area
  const uint32_t t0 = millis();
  AlbumRecord album;
  ArtistRecord artist;
  const bool found = findAlbumOf(current, album, artist);
  const uint32_t findMs = millis() - t0;
  String albumName, artistName, info;
  ArtDrawResult art;
  if (found) {
    albumName = String(album.name, strnlen(album.name, sizeof(album.name)));
    artistName = String(artist.name, strnlen(artist.name, sizeof(artist.name)));
    const String jpg = artCoverPath(artistName.c_str(), albumName.c_str());
    String marker = jpg;
    marker.replace(".jpg", ".no");
    FILINFO fi;
    lcdFillRect(0, 0, SCREEN_W, COVER_SIZE, C_BG);
    if (f_stat(cardPath(jpg).c_str(), &fi) == FR_OK) {
      art = artDrawJpeg(cardPath(jpg), 0, 0, COVER_SIZE, COVER_SIZE);
      info = art.ok ? String(art.width) + "x" + art.height + (art.scale ? " 1/" + String(1 << art.scale) : "") + ", " +
                          String((art.bytes + 512) / 1024) + " KB, " + art.ms + " ms"
                    : String("Can't show: ") + art.error;
    } else if (f_stat(cardPath(marker).c_str(), &fi) == FR_OK) {
      info = "No cover found online";
    } else {
      info = "Not downloaded yet";
    }
    Serial.printf("[cover] \"%s\" by \"%s\": %s (%s); album found in %lu ms\n", albumName.c_str(),
                  artistName.c_str(), jpg.c_str(), info.c_str(), findMs);
  } else {
    info = current.album.isEmpty() ? "No album tag" : "Album not in the library";
    Serial.printf("[cover] album of song %lu not found (%lu ms)\n", current.id, findMs);
  }
  if (!art.ok) {  // placeholder
    lcdFillRect(0, 0, SCREEN_W, COVER_SIZE, C_HEADER);
    canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_HEADER);
    drawCenteredText("No cover", SCREEN_W / 2, 14, 2, C_DIM);
    lcdBlit(0, COVER_SIZE / 2 - CANVAS_H / 2, SCREEN_W, CANVAS_H);
  }

  // Names under the cover, then the test details.
  canvas->fillRect(0, 0, SCREEN_W, CANVAS_H, C_BG);
  canvas->setTextSize(1);
  canvas->setTextColor(C_TEXT);
  canvas->setCursor(8, 6);
  canvas->print(fitText(current.title, 37));
  canvas->setTextColor(C_TEXT);
  canvas->setCursor(8, 18);
  canvas->print(fitText(found ? albumName : current.album, 37));
  canvas->setTextColor(C_ACCENT);
  canvas->setCursor(8, 30);
  canvas->print(fitText(found ? artistName : current.artist, 37));
  lcdBlit(0, COVER_SIZE, SCREEN_W, CANVAS_H);
  canvas->fillRect(0, 0, SCREEN_W, SCREEN_H - COVER_SIZE - CANVAS_H, C_BG);
  canvas->setTextColor(C_DIM);
  canvas->setCursor(8, 4);
  canvas->print(fitText(info, 37));
  canvas->setCursor(8, 20);
  canvas->print("Tap to close");
  lcdBlit(0, COVER_SIZE + CANVAS_H, SCREEN_W, SCREEN_H - COVER_SIZE - CANVAS_H);
  lcdSetMuted(true);
}

static void openCover() {
  stopList();
  coverOpen = true;
  drawCover();
}

// Ends the cover view without drawing (another screen takes over).
static void dropCover() {
  if (coverOpen) {
    coverOpen = false;
    lcdSetMuted(false);
    lcdScrollOffset(listOffset);  // may have moved while muted
  }
}

static void closeCover() {
  dropCover();
  uiShowPlayer();
}

// ================================================================ Actions
static void startPlayback() {
  uint32_t id;
  if (!ctxSong(playCtx, playIndex, id) || !Catalog::song(id, current)) {
    playIndex = -1;
    drawNowPlaying();
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
  drawNowPlaying();
  drawControlIcon(2, false);
}

static void playFromLevel(const Level &l, uint32_t songIndex) {
  playCtx = levelContext(l);
  playIndex = songIndex;
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
  drawNowPlaying();
  drawControlIcon(2, false);
}

static void nextTrack() {
  if (playIndex >= 0 && playCtx.count > 0) {
    playIndex = (playIndex + 1) % playCtx.count;
    startPlayback();
  }
}

static void previousTrack() {
  if (playIndex < 0 || playCtx.count == 0) {
    return;
  }
  // Restart the current song if we're more than a few seconds in.
  if (!(audioState() != STOPPED && audioFilePos() > 64 * 1024)) {
    playIndex = playIndex == 0 ? playCtx.count - 1 : playIndex - 1;
  }
  startPlayback();
}

static void handleTrackEnded() {
  Serial.println("Track finished");
  if (playIndex >= 0 && static_cast<uint32_t>(playIndex) + 1 < playCtx.count) {
    playIndex++;
    startPlayback();
  } else {
    drawNowPlaying();
    drawControlIcon(2, false);
  }
}

static void scrollTo(int32_t top) {
  const int32_t count = levelCount(level());
  top = constrain(top, 0, max<int32_t>(0, count - 1));
  if (top != level().top) {
    level().top = top;
    drawList();
  }
}

static void pageList(int direction) {
  const int32_t top = level().top + direction * VISIBLE_ROWS;
  if (top < levelCount(level())) {
    scrollTo(max<int32_t>(0, top));
  }
}

static int lastStripBucket = -1;

static void stripJump(int16_t y) {
  const JumpTable *j = levelJump(level());
  if (!j) {
    return;
  }
  int b = constrain((y - LIST_Y) / STRIP_PITCH, 0, 26);
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
  drawNav();
  drawList();
}

static void goBack() {
  Tab &tab = tabs[activeTab];
  if (tab.depth > 1) {
    tab.depth--;
    drawNav();
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
  drawNav();
  drawList();
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
    default: break;
  }
}

static bool stripDrag = false;
static uint32_t statusHoldStart = 0;  // nonzero while the status corner is held
static bool rebuildRequested = false;
#define REBUILD_HOLD_MS 2000

bool uiTakeRebuildRequest() {
  const bool requested = rebuildRequested;
  rebuildRequested = false;
  return requested;
}

void uiHandleTouch(TouchPhase phase, int16_t x, int16_t y) {
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
  if (coverOpen) {
    closeCover();
    return;
  }
  if (y >= LIST_END && y < CTRL_Y && playIndex >= 0) {  // now-playing panel
    openCover();
    return;
  }
  if (y >= LIST_Y && y < LIST_END && x < LIST_W) {
    listTouchDown(y);  // a tap fires on release if the finger didn't move
    return;
  }
  stopList();  // anything else first ends list movement
  if (y < TAB_H) {
    if (x < STATUS_X) {
      selectTab(x / TAB_W);
    } else if (libraryStatus.isEmpty()) {  // not while an update is shown there
      statusHoldStart = millis() | 1;
      uiSetLibraryStatus("Hold to\nrebuild");
    }
    return;
  }
  if (y < HEADER_H) {
    if (x < BACK_W + 4) {
      goBack();
    } else if (x >= VOL_DOWN_X && x < VOL_TEXT_X) {
      audioSetVolume(audioVolume() - 1);
      drawNav();
    } else if (x >= VOL_UP_X - 4) {
      audioSetVolume(audioVolume() + 1);
      drawNav();
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
  if (y >= CTRL_Y) {
    const int button = constrain(x / CTRL_W, 0, CTRL_BUTTONS - 1);
    drawControlIcon(button, true);
    switch (button) {
      case 0: pageList(-1); break;
      case 1: previousTrack(); break;
      case 2: togglePlayPause(); break;
      case 3: nextTrack(); break;
      case 4: pageList(1); break;
    }
    drawControlIcon(button, false);
  }
}

void uiSetLibraryStatus(const String &text) {
  if (text == libraryStatus) {
    return;
  }
  libraryStatus = text;
  composeLibraryStatus(0);
  lcdBlit(STATUS_X, 0, STATUS_W, TAB_H);
}

void uiLibraryChanged() {
  // Ids changed. Find the playing song again by its path; it continues in
  // the "all songs" order. Navigation goes back to each tab's top.
  if (playIndex >= 0) {
    uint32_t pos;
    if (Catalog::findTitlePos(currentPath, current.title, pos)) {
      playCtx = {CTX_TITLE, 0, Catalog::songCount()};
      playIndex = pos;
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
  if (audioTakeTrackEnded()) {
    handleTrackEnded();
  }
  if (statusHoldStart && millis() - statusHoldStart >= REBUILD_HOLD_MS) {
    statusHoldStart = 0;
    rebuildRequested = true;
  }

  if (coverOpen && playIndex >= 0 && current.id != coverSongId) {
    drawCover();  // the next song started: maybe another album
  }

  tickList();
  if (!listMoving()) {
    marqueeTick(listMarquee);
  }
  marqueeTick(titleMarquee);
  marqueeTick(artistMarquee);

  static uint32_t lastProgressDraw = 0;
  if (audioState() == PLAYING && millis() - lastProgressDraw > 500) {
    lastProgressDraw = millis();
    drawProgress(false);
    drawTime(false);
  }
}
