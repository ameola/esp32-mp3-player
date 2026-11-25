#include "ArtFetch.h"

#include <atomic>
#include <functional>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <new>
#include <time.h>

#include "ArtScan.h"
#include "ArtView.h"
#include "AudioPlayer.h"
#include "CardFile.h"
#include "Catalog.h"
#include "Config.h"
#include "LibFormat.h"
#include "LibClean.h"
#include "LibStore.h"
#include "LyricStore.h"
#include "Library.h"
#include "Text.h"
#include "ThumbStore.h"
#include "UiTheme.h"
#include "WifiSetup.h"

namespace {

#define ART_DIR LIB_DIR "/art"
#define IDLE_BEFORE_MS 10000         // nothing playing this long: start a pass
#define RETRY_AFTER_MS (10 * 60000)  // after a network failure
#define MISSING_RETRY_DAYS 30
// MusicBrainz allows one request per second, but answered half of them 503
// (busy) at 1.1 s apart.
#define MB_INTERVAL_MS 2000
// Deezer (artist photos) allows 50 requests per 5 seconds.
#define DZ_INTERVAL_MS 500
// LRCLIB (lyrics): after a burst of about 60 requests it answers every
// other one 429 at 0.6 s apart. The gap starts short and grows on a 429.
#define LRC_INTERVAL_MS 250
#define LRC_INTERVAL_MAX_MS 3000
#define LYRICS_MAX 12288             // longest text kept (as the player reads .lrc files)
#define WANT_MAX 8                   // songs to look up lyrics for first
#define MIN_FREE_FOR_WIFI 90000      // WiFi ~43 KB + TLS ~45 KB
#define HTTP_TIMEOUT_MS 5000         // also bounds how long a stop request waits
#define TASK_STACK 8192              // TLS handshake runs on it
// Pause between card lookups of the steps that use no network (artist
// stand-ins, thumbnails), so the list on screen still gets the card: back
// to back they made each of its thumbnail reads take 30-70 ms, not 5-10.
#define CARD_PAUSE_MS 10

// O_SKIPPED: the search answered but the picture's host couldn't be read
// (archive.org's HTTPS nodes need more memory than the player has while
// its UI runs); tried again next pass, and not a network failure.
enum Outcome { O_SAVED, O_NONE, O_NET_ERROR, O_STOPPED, O_SKIPPED };

bool enabled = false;
TaskHandle_t task = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<uint32_t> progressDone{0}, progressTotal{0};
uint32_t idleSince = 0;
uint32_t retryAt = 0;
uint32_t cursor = 0;              // next album to look at (survives stopped passes)
uint32_t cursorGen = LIB_NONE;    // generation the cursor belongs to
uint32_t finishedGen = LIB_NONE;  // every album of this generation was handled
uint32_t cursorSongs = 0;          // songs (lyrics steps) ahead of the pictures when the cursor was set
bool albumsSkipped = false;       // artFetchArtistsNow: this pass started past the albums
uint32_t lastMbRequest = 0;
uint32_t lastDzRequest = 0;
ThumbStore store;  // the pass's handle (the UI has its own)
uint16_t thumbPixels[ART_THUMB * ART_THUMB];
uint32_t passGen = LIB_NONE;
std::atomic<uint32_t> picturesFrom{0};  // cursor where the picture steps begin (after lyrics)
std::atomic<uint32_t> lyricsSaved{0};
// Songs whose lyrics the player asked for (the playing one): first.
portMUX_TYPE wantLock = portMUX_INITIALIZER_UNLOCKED;
uint32_t wanted[WANT_MAX];
uint8_t wantCount = 0;
uint32_t wantGen = LIB_NONE;

uint32_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

// Sleeps up to ms; false if a stop was requested.
bool nap(uint32_t ms) {
  for (uint32_t t0 = millis(); millis() - t0 < ms;) {
    if (stopRequested) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return !stopRequested;
}

// ---------------------------------------------------------------- names
// Keys before names were normalized (FNV-1a of the names as shown, case
// folded); only used to move pictures saved under them (migrateKeys()).
uint32_t legacyAlbumKey(const char *artist, const char *album) {
  uint32_t h = 2166136261u;  // FNV-1a, case-insensitive
  auto mix = [&h](const char *s) {
    for (; *s; s++) {
      h ^= static_cast<uint8_t>(tolower(static_cast<unsigned char>(*s)));
      h *= 16777619u;
    }
  };
  mix(artist);
  h ^= 0x1f;
  h *= 16777619u;
  mix(album);
  return h;
}

uint32_t legacyArtistKey(const char *artist) {
  return legacyAlbumKey(artist, "\x01");
}

// A picture's key: a hash of the album artist and album name, normalized
// like the library groups them (LibClean), so "KoRn" and "Korn", or an
// album's "Disc 1" and "Disc 2", share one.
uint32_t albumKey(const char *artist, const char *album) {
  return legacyAlbumKey(libNormName(artist, true).c_str(), libNormName(album, false).c_str());
}

// An artist's photo: a key no album has (album names have no \x01).
uint32_t artistKey(const char *artist) {
  return albumKey(artist, "\x01");
}

String keyPath(uint32_t key, const char *ext) {
  char p[64];
  snprintf(p, sizeof(p), ART_DIR "/%02x/%08lx.%s", static_cast<unsigned>(key >> 24),
           static_cast<unsigned long>(key), ext);
  return p;
}

// Names taggers and the indexer use for a missing tag.
bool placeholder(const String &name) {
  static const char *const names[] = {"", "Unknown", "<Unknown>", "[unknown]", "Unknown Album", "Unknown Artist"};
  for (const char *n : names) {
    if (name.equalsIgnoreCase(n)) return true;
  }
  return false;
}

// Album name for the search: without "(Disc 1)", "[Bonus Tracks]" and the
// like, which MusicBrainz release groups don't carry.
String searchName(String name) {
  for (;;) {
    name.trim();
    const int n = name.length();
    if (n && (name[n - 1] == ')' || name[n - 1] == ']')) {
      const int open = name.lastIndexOf(name[n - 1] == ')' ? '(' : '[');
      if (open > 0) {
        name.remove(open);
        continue;
      }
    }
    String lower = name;
    lower.toLowerCase();
    const int disc = max(lower.lastIndexOf(" disc "), lower.lastIndexOf(" cd "));
    if (disc > 0 && isdigit(static_cast<unsigned char>(lower[lower.length() - 1]))) {
      name.remove(disc);
      continue;
    }
    return name;
  }
}

String urlEncode(const String &s) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

String phrase(const String &s) {  // a quoted Lucene phrase
  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '"' || s[i] == '\\') out += '\\';
    out += s[i];
  }
  return out + "\"";
}

// A name reduced for comparing card tags with MusicBrainz: lower case, "&"
// and "+" as "and", punctuation (also typographic quotes and dashes) gone,
// "vol"/"volume" dropped, single spaces.
String comparable(const String &s, bool dropLeadingThe) {
  String words;
  auto space = [&] {
    if (words.length() && words[words.length() - 1] != ' ') words += ' ';
  };
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = s[i];
    if (c == 0xE2 && i + 2 < s.length() && static_cast<uint8_t>(s[i + 1]) == 0x80) {
      // U+2000..U+203F: ’ ‘ “ ” – — … and friends. ’ and ‘ go like ',
      // "That’s" is "That's" is "Thats".
      const uint8_t c2 = s[i + 2];
      i += 2;
      if (c2 != 0x98 && c2 != 0x99) space();
    } else if (c == '&' || c == '+') {
      space();
      words += "and ";
    } else if (c >= 0x80 || isalnum(c)) {
      words += static_cast<char>(tolower(c));
    } else if (c != '\'') {  // "Rock 'n' Roll" and "Rock n Roll" alike
      space();
    }
  }
  words.trim();
  String out;
  for (int start = 0; start < static_cast<int>(words.length());) {
    int end = words.indexOf(' ', start);
    if (end < 0) end = words.length();
    const String w = words.substring(start, end);
    const bool drop = w == "vol" || w == "volume" || (dropLeadingThe && start == 0 && w == "the");
    if (!drop) out += (out.length() ? " " : "") + w;
    start = end + 1;
  }
  return out;
}

int wordCount(const String &s) {
  int n = s.length() ? 1 : 0;
  for (size_t i = 0; i < s.length(); i++) n += s[i] == ' ';
  return n;
}

String withoutLastWord(const String &s) {
  const int sp = s.lastIndexOf(' ');
  return sp > 0 ? s.substring(0, sp) : s;
}

// The card's album name for comparing: a name the library cut at 43
// characters loses its last, maybe partial, word ("...Collection D").
String cardTitle(const String &album, bool truncated) {
  const String a = comparable(searchName(album), false);
  return truncated ? withoutLastWord(a) : a;
}

// Words of the album name a loose search requires (AND), without the ones
// MusicBrainz titles often spell differently ("&", "+", "and", "Vol.") and
// words with an apostrophe (MusicBrainz finds "ain't", not "aint" or "ain").
String titleQuery(const String &album, bool truncated) {
  String name = searchName(album);
  name.trim();
  if (truncated) name = withoutLastWord(name);
  String q;
  for (int start = 0; start < static_cast<int>(name.length());) {
    int end = name.indexOf(' ', start);
    if (end < 0) end = name.length();
    const String token = name.substring(start, end);
    start = end + 1;
    if (token.indexOf('\'') >= 0 || token.indexOf("\xE2\x80\x99") >= 0) continue;
    const String words = comparable(token, false);  // "Up-Up" is two words
    for (int s = 0; s < static_cast<int>(words.length());) {
      int e = words.indexOf(' ', s);
      if (e < 0) e = words.length();
      const String w = words.substring(s, e);
      s = e + 1;
      if (w != "and" && w != "the") q += (q.length() ? " AND " : "") + w;
    }
  }
  return q;
}

// How well a MusicBrainz title fits the card's album name: 2 the same
// (after comparable()), 1 one is the other plus more words, 0 no. The
// "plus more words" cases: a name the library cut short; "Greatest Hits,
// Vol. 1" for "Greatest Hits 1 1962-1965"; "You Ain't Heard Nothin' Yet:
// Jolie's Finest" for "You Ain't Heard Nothin' Yet!". Short names must match
// exactly ("Greatest Hits" isn't "Greatest Hits 2").
int titleFit(const String &album, const String &mb, bool truncated) {
  const String a = cardTitle(album, truncated);
  const String b = comparable(searchName(mb), false);
  if (a.isEmpty()) return 0;
  if (a == b) return 2;
  if (b.startsWith(a + " ") && (truncated || wordCount(a) >= 3)) return 1;
  if (a.startsWith(b + " ") && wordCount(b) >= 4) return 1;
  return 0;
}

String noSpaces(String s) {
  s.replace(" ", "");
  return s;
}

// Names that may have been cut short: at the library's 43 or 47 bytes, or
// at the 30 characters of an ID3v1 tag ("Ben Harper and the Innocent Cri").
bool maybeTruncated(const String &name, size_t fieldBytes) {
  return name.length() >= fieldBytes - 2 || name.length() == 30 || name.length() == 31;
}

bool sameArtist(const String &card, const String &mb) {  // "2 Pac" is "2Pac"
  const String a = comparable(card, true), b = comparable(mb, true);
  if (a.isEmpty()) return false;
  if (noSpaces(a) == noSpaces(b)) return true;
  // "Ben Harper and the Innocent Cri": "Ben Harper & The Innocent Criminals".
  const String start = withoutLastWord(a);
  return maybeTruncated(card, sizeof(ArtistRecord::name)) && wordCount(start) >= 2 && b.startsWith(start + " ");
}

// One release group of a search answer.
struct Match {
  String id, title, artist;  // artist: the first name of its artist credit
  int score = 0;
};
#define MAX_MATCHES 3

// Reads the release groups of a search answer as it streams in, keeping
// only what Match needs: groups carry aliases, tags and every release, a few
// KB each.
class GroupReader {
 public:
  Match matches[MAX_MATCHES];
  int count = 0;

  void feed(const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) step(static_cast<char>(d[i]));
  }

 private:
  // Depth 1 is the answer, 2 the "release-groups" array, 3 one group;
  // the artist credit's first name sits at depth 5.
  int depth = 0;
  bool inString = false, escaped = false, afterColon = false;
  bool inGroups = false, inCredit = false;
  String text, key;
  Match cur;
  bool inGroup = false;

  void step(char c) {
    if (inString) {
      if (escaped) {
        escaped = false;
        if (text.length() < 120) text += c;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
        if (afterColon) value(text);
        else key = text;
        afterColon = false;
      } else if (text.length() < 120) {
        text += c;
      }
      return;
    }
    switch (c) {
      case '"': inString = true; text = ""; break;
      case ':': afterColon = true; break;
      case ',': afterColon = false; break;
      case '{':
      case '[':
        if (depth == 1 && c == '[') inGroups = key == "release-groups";
        if (depth == 2 && c == '{' && inGroups) {
          cur = Match();
          inGroup = true;
        }
        if (depth == 3 && inGroup) inCredit = key == "artist-credit";
        depth++;
        afterColon = false;
        break;
      case '}':
      case ']':
        depth--;
        if (depth == 3) inCredit = false;
        if (depth == 2 && c == '}' && inGroup) {
          inGroup = false;
          if (count < MAX_MATCHES) matches[count++] = cur;
        }
        if (depth == 1) inGroups = false;
        break;
      default:
        if (afterColon && inGroup && depth == 3 && key == "score" && isdigit(static_cast<unsigned char>(c))) {
          cur.score = cur.score * 10 + (c - '0');
        }
    }
  }

  void value(const String &v) {
    if (!inGroup) return;
    if (depth == 3) {
      if (key == "id") cur.id = v;
      else if (key == "title") cur.title = v;
    } else if (depth == 5 && inCredit && key == "name" && cur.artist.isEmpty()) {
      cur.artist = v;
    }
  }
};

// One artist of a Deezer search answer.
struct ArtistMatch {
  String name;     // display text (accents folded, like the card's names)
  String picture;  // 250 px photo URL
};
#define MAX_ARTISTS 5

// Reads {"data":[{"name":..,"picture_medium":..},..]} as it streams in.
// Deezer escapes non-ASCII letters (\u00e9); raw UTF-8 is handled too.
class ArtistReader {
 public:
  ArtistMatch matches[MAX_ARTISTS];
  int count = 0;
  bool error = false;  // {"error":{..}}: e.g. over the request quota

  void feed(const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) step(static_cast<char>(d[i]));
  }

 private:
  int depth = 0;
  bool inString = false, escaped = false, afterColon = false, inData = false, inItem = false;
  int hexLeft = 0;
  uint32_t cp = 0;
  String text, key;
  ArtistMatch cur;

  void add(char c) {
    if (text.length() < 200) text += c;
  }
  void addCodepoint(uint32_t c) {  // as UTF-8, decoded with the rest
    if (c < 0x80) {
      add(c);
    } else if (c < 0x800) {
      add(0xC0 | (c >> 6));
      add(0x80 | (c & 63));
    } else {
      add(0xE0 | (c >> 12));
      add(0x80 | ((c >> 6) & 63));
      add(0x80 | (c & 63));
    }
  }

  void step(char c) {
    if (inString) {
      if (hexLeft) {
        cp = cp * 16 + (isdigit(static_cast<unsigned char>(c)) ? c - '0' : (tolower(c) - 'a' + 10) & 15);
        if (--hexLeft == 0) addCodepoint(cp);
      } else if (escaped) {
        escaped = false;
        if (c == 'u') {
          hexLeft = 4;
          cp = 0;
        } else {
          add(c == 'n' || c == 't' || c == 'r' ? ' ' : c);
        }
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
        if (afterColon) value();
        else key = text;
        afterColon = false;
      } else {
        add(c);
      }
      return;
    }
    switch (c) {
      case '"': inString = true; text = ""; break;
      case ':': afterColon = true; break;
      case ',': afterColon = false; break;
      case '{':
      case '[':
        if (depth == 1 && c == '[') inData = key == "data";
        if (depth == 1 && c == '{' && afterColon && key == "error") error = true;
        if (depth == 2 && c == '{' && inData) {
          cur = ArtistMatch();
          inItem = true;
        }
        depth++;
        afterColon = false;
        break;
      case '}':
      case ']':
        depth--;
        if (depth == 2 && c == '}' && inItem) {
          inItem = false;
          if (count < MAX_ARTISTS) matches[count++] = cur;
        }
        if (depth == 1) inData = false;
        break;
    }
  }

  void value() {
    if (!inItem || depth != 3) return;
    if (key == "name") cur.name = decodeUtf8(reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
    else if (key == "picture_medium") cur.picture = text;
  }
};

// ---------------------------------------------------------------- WiFi
// ESP-IDF directly, with fewer buffers than the Arduino WiFi class uses (it
// can't be configured): no A-MPDU aggregation, fewer RX/TX buffers, no
// Arduino event task. About 43 KB while connected.
EventGroupHandle_t wifiEvents = nullptr;
esp_netif_t *staNetif = nullptr;
bool wifiUp = false;
bool ownNetif = false, ownLoop = false;  // created here (not by the Arduino WiFi setup screen)
#define GOT_IP BIT0

void onWifiEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifiEvents, GOT_IP);
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupClearBits(wifiEvents, GOT_IP);
}

bool wifiStart() {
  String ssid, pass;
  if (!wifiSavedCredentials(ssid, pass)) return false;
  esp_netif_init();  // once; lwIP's task stays (it can't be stopped)
  ownLoop = esp_event_loop_create_default() == ESP_OK;  // else it exists already
  if (!wifiEvents) wifiEvents = xEventGroupCreate();
  staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  ownNetif = !staNetif;
  if (ownNetif) staNetif = esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.static_rx_buf_num = 2;
  cfg.dynamic_rx_buf_num = 6;
  cfg.static_tx_buf_num = 0;
  cfg.tx_buf_type = 1;  // dynamic
  cfg.dynamic_tx_buf_num = 8;
  cfg.cache_tx_buf_num = 2;
  cfg.ampdu_rx_enable = 0;
  cfg.ampdu_tx_enable = 0;
  cfg.nvs_enable = 0;
  wifiUp = true;  // from here on wifiStop() cleans up (its calls are safe on a failed init)
  if (esp_wifi_init(&cfg) != ESP_OK) return false;
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  wifi_config_t wc = {};
  strlcpy(reinterpret_cast<char *>(wc.sta.ssid), ssid.c_str(), sizeof(wc.sta.ssid));
  strlcpy(reinterpret_cast<char *>(wc.sta.password), pass.c_str(), sizeof(wc.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  esp_wifi_start();
  // Power save holds received packets until the next beacon: every round trip
  // of a TLS handshake waited for it.
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_connect();
  const uint32_t t0 = millis();
  while (!(xEventGroupGetBits(wifiEvents) & GOT_IP)) {
    if (stopRequested || millis() - t0 > 20000) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  // Certificates are checked against dates (and .no markers age by them).
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  while (time(nullptr) < 1700000000 && millis() - t0 < 35000 && !stopRequested) vTaskDelay(pdMS_TO_TICKS(100));
  Serial.printf("[art] WiFi connected to %s in %lu ms, clock %s, free %u\n", ssid.c_str(), millis() - t0,
                time(nullptr) > 1700000000 ? "set" : "NOT set", freeHeap());
  return time(nullptr) > 1700000000;
}

void wifiStop() {
  if (!wifiUp) return;
  esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent);
  esp_wifi_disconnect();
  esp_wifi_stop();
  esp_wifi_deinit();
  xEventGroupClearBits(wifiEvents, GOT_IP);
  // Give back what was created for this pass (about 12 KB with the event task).
  if (ownNetif && staNetif) {
    esp_netif_destroy_default_wifi(staNetif);
    staNetif = nullptr;
    ownNetif = false;
  }
  if (ownLoop) {
    esp_event_loop_delete_default();
    ownLoop = false;
  }
  wifiUp = false;
}

// ---------------------------------------------------------------- HTTP
// GET with redirects followed by hand (so a stop request is seen between
// reads). sink() gets the body of the final 200 answer; returns the status
// (0 on a connection error, -1 when stopped).
uint8_t httpBuf[1024];
bool logTiming = true;

template <typename Sink>
int httpGet(const String &url, Sink sink) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.user_agent = ART_USER_AGENT;
  cfg.timeout_ms = HTTP_TIMEOUT_MS;
  cfg.buffer_size = 1536;
  cfg.buffer_size_tx = 1024;
  cfg.disable_auto_redirect = true;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return 0;
  int status = 0;
  String timing;
  for (int hop = 0; hop < 6 && !stopRequested; hop++) {
    const uint32_t t0 = millis();
    char url[256] = "";
    esp_http_client_get_url(c, url, sizeof(url));  // for the timing log: scheme and host
    String host = url;
    const int slash = host.indexOf('/', host.indexOf("//") + 2);
    if (slash > 0) host.remove(slash);
    if (esp_http_client_open(c, 0) != ESP_OK) {
      status = 0;
      break;
    }
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    timing += " " + host + " " + status + " in " + (millis() - t0) + " ms;";
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      esp_http_client_flush_response(c, nullptr);
      if (esp_http_client_set_redirection(c) != ESP_OK) break;
      continue;
    }
    if (status == 200) {
      int n;
      while ((n = esp_http_client_read(c, reinterpret_cast<char *>(httpBuf), sizeof(httpBuf))) > 0) {
        if (stopRequested || !sink(httpBuf, n)) {
          status = stopRequested ? -1 : 0;
          break;
        }
      }
      if (n < 0) status = 0;
      timing += String(" body done at ") + (millis() - t0) + " ms";
    }
    break;
  }
  if (logTiming) Serial.printf("[art]   http:%s\n", timing.c_str());
  if (stopRequested) status = -1;
  esp_http_client_cleanup(c);
  return status;
}

// ---------------------------------------------------------------- one album
// A "no cover" marker's size is the version of the search that wrote it;
// smaller ones are retried once. 0: exact phrases only, which missed many
// albums. 2: before curly apostrophes and cut-short artists were handled.
#define MARKER_SIZE 3

bool writeMarker(uint32_t key) {
  FIL *f = cardNewFil();
  bool ok = f && f_open(f, cardPath(keyPath(key, "no")).c_str(), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) {  // its date is what counts
    UINT w = 0;
    ok = cardWrite(f, "v3\n", MARKER_SIZE, &w) == FR_OK && w == MARKER_SIZE;
    f_close(f);
  }
  cardDeleteFil(f);
  return ok;
}

// One MusicBrainz release group search (plain HTTP: no TLS needed). Returns
// the HTTP status, -1 if stopped.
int mbSearch(const String &query, int limit, GroupReader &reader) {
  const String url = "http://musicbrainz.org/ws/2/release-group/?fmt=json&limit=" + String(limit) +
                     "&query=" + urlEncode(query);
  int status = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!nap(MB_INTERVAL_MS - min<uint32_t>(MB_INTERVAL_MS, millis() - lastMbRequest))) return -1;
    lastMbRequest = millis();
    reader = GroupReader();
    status = httpGet(url, [&](const uint8_t *d, int n) {
      reader.feed(d, n);
      return true;  // read the rest so the connection ends cleanly
    });
    if (status != 503) break;  // busy: MusicBrainz asks clients to slow down
    if (!nap(3000 * (attempt + 1))) return -1;
  }
  return status;
}

// Downloads an image to "<key>.jpg" (through "<key>.tmp") and makes its
// list thumbnail. Returns the HTTP status: 200 saved, -1 stopped, 0 also
// for card errors; `info` describes what was saved.
int saveImage(const String &url, uint32_t key, String &info) {
  const String tmp = keyPath(key, "tmp");
  FIL *f = cardNewFil();
  if (!f || f_open(f, cardPath(tmp).c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    cardDeleteFil(f);
    return 0;
  }
  uint32_t bytes = 0;
  bool writeOk = true;
  const uint32_t t0 = millis();
  int status = httpGet(url, [&](const uint8_t *d, int n) {
    UINT w = 0;
    writeOk = cardWrite(f, d, n, &w) == FR_OK && w == static_cast<UINT>(n);
    bytes += n;
    return writeOk && bytes < 512 * 1024;
  });
  f_close(f);
  cardDeleteFil(f);
  if (status == 200 && writeOk && bytes) {
    const String jpg = keyPath(key, "jpg");
    f_unlink(cardPath(jpg).c_str());
    if (f_rename(cardPath(tmp).c_str(), cardPath(jpg).c_str()) != FR_OK) return 0;
    CardFile cf;
    uint16_t w = 0, h = 0;
    const char *kind = cf.open(jpg) ? artDescribeImage(cf, 0, cf.size(), w, h) : "?";
    cf.close();
    const bool thumb = artMakeThumb(cardPath(jpg), thumbPixels) && store.write(key, thumbPixels);
    if (!thumb) Serial.println("[art]   (no thumbnail: can't decode it?)");
    info = String(bytes) + " bytes, " + kind + " " + w + "x" + h + ", " + (millis() - t0) + " ms";
    return 200;
  }
  f_unlink(cardPath(tmp).c_str());
  return status == 200 ? 0 : status;
}

Outcome fetchAlbum(uint32_t key, const String &album, const String &artist) {
  // 1a. Search for the exact names.
  String query = "releasegroup:" + phrase(searchName(album));
  query += " AND artist:" + phrase(artist);
  GroupReader *reader = new (std::nothrow) GroupReader();
  if (!reader) return O_NET_ERROR;
  int status = mbSearch(query, 1, *reader);
  Match best;
  if (status == 200 && reader->count && reader->matches[0].score >= 90) best = reader->matches[0];
  // 1b. Nothing: search for the words, then check the names ourselves.
  // Catches "Pleasure & Pain" (MusicBrainz: "Pleasure + Pain"), "The Bay
  // City Rollers" ("Bay City Rollers") and names the library cut short.
  // 1c. Still nothing and the name has a subtitle: search for the part
  // before it ("You Ain't Heard Nothin' Yet: Jolie's Finest" is "You Ain't
  // Heard Nothin' Yet!").
  const bool truncated = maybeTruncated(album, sizeof(AlbumRecord::name));
  const int colon = album.indexOf(':');
  // The artist's words are optional (OR), also run together: "The Bay City
  // Rollers" still finds "Bay City Rollers", "2 Pac" finds "2Pac".
  // sameArtist() checks the result.
  String artistWords = comparable(artist, true);
  if (artistWords.indexOf(' ') > 0) artistWords += " " + noSpaces(artistWords);
  for (int pass = 0; pass < 2 && status == 200 && best.id.isEmpty(); pass++) {
    const String words = pass == 0 ? titleQuery(album, truncated)
                         : colon > 0 ? titleQuery(album.substring(0, colon), false)
                                     : String();
    if (words.isEmpty()) continue;
    query = "releasegroup:(" + words + ") AND artist:(" + artistWords + ")";
    status = mbSearch(query, MAX_MATCHES, *reader);
    int bestFit = 0;
    for (int i = 0; status == 200 && i < reader->count; i++) {
      const Match &m = reader->matches[i];
      if (!sameArtist(artist, m.artist)) continue;
      const int fit = titleFit(album, m.title, truncated);
      if (fit > bestFit) {
        bestFit = fit;
        best = m;
      }
    }
    if (best.id.length()) Serial.printf("[art]   loose match \"%s\" by \"%s\"\n", best.title.c_str(), best.artist.c_str());
  }
  const String bestSeen = reader->count ? reader->matches[0].title : String();
  delete reader;
  if (status == -1) return O_STOPPED;
  if (status != 200) {
    Serial.printf("[art]   search failed: HTTP %d\n", status);
    return O_NET_ERROR;
  }
  const String id = best.id, title = best.title;
  if (id.length() != 36) {
    Serial.printf("[art]   no match (closest \"%s\")\n", bestSeen.c_str());
    return writeMarker(key) ? O_NONE : O_NET_ERROR;
  }

  // 2. Cover: coverartarchive.org redirects to archive.org, which needs HTTPS.
  const uint32_t t0 = millis();
  String info;
  status = saveImage("http://coverartarchive.org/release-group/" + id + "/front-250", key, info);
  if (status == 200) {
    Serial.printf("[art]   saved \"%s\": %s; free %u\n", title.c_str(), info.c_str(), freeHeap());
    return O_SAVED;
  }
  if (status == -1) return O_STOPPED;
  if (status == 404) {  // the release group has no front cover
    Serial.printf("[art]   \"%s\" has no cover\n", title.c_str());
    return writeMarker(key) ? O_NONE : O_NET_ERROR;
  }
  Serial.printf("[art]   cover failed: HTTP %d after %lu ms; free %u\n", status, millis() - t0, freeHeap());
  return status == 0 ? O_SKIPPED : O_NET_ERROR;
}

// One Deezer artist search. Returns the HTTP status, -1 if stopped.
int dzSearch(const String &artist, ArtistReader &reader) {
  const String url = "http://api.deezer.com/search/artist?limit=" + String(MAX_ARTISTS) + "&q=" + urlEncode(artist);
  int status = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!nap(DZ_INTERVAL_MS - min<uint32_t>(DZ_INTERVAL_MS, millis() - lastDzRequest))) return -1;
    lastDzRequest = millis();
    reader = ArtistReader();
    status = httpGet(url, [&](const uint8_t *d, int n) {
      reader.feed(d, n);
      return true;
    });
    if (status == 200 && !reader.error) break;
    if (status == 200) status = 429;  // an error answer: over the quota
    if (!nap(5000 * (attempt + 1))) return -1;
  }
  return status;
}

// An artist's photo: the first search result with the same name (after
// comparable(), as for albums). Deezer has no photo for some artists; their
// picture URL then has an empty id ("/images/artist//250x250...").
Outcome fetchArtist(uint32_t key, const String &artist) {
  ArtistReader *reader = new (std::nothrow) ArtistReader();
  if (!reader) return O_NET_ERROR;
  int status = dzSearch(artist, *reader);
  String picture, found;
  for (int i = 0; status == 200 && i < reader->count; i++) {
    const ArtistMatch &m = reader->matches[i];
    if (!sameArtist(artist, m.name)) continue;
    found = m.name;
    if (m.picture.startsWith("http") && m.picture.indexOf("/artist//") < 0) picture = m.picture;
    break;
  }
  const String closest = reader->count ? reader->matches[0].name : String();
  delete reader;
  if (status == -1) return O_STOPPED;
  if (status != 200) {
    Serial.printf("[art]   artist search failed: HTTP %d\n", status);
    return O_NET_ERROR;
  }
  if (picture.isEmpty()) {
    const String why = found.length() ? String("Deezer has none") : "no match (closest \"" + closest + "\")";
    Serial.printf("[art]   no photo: %s\n", why.c_str());
    return writeMarker(key) ? O_NONE : O_NET_ERROR;
  }
  const uint32_t t0 = millis();
  String info;
  status = saveImage(picture, key, info);
  if (status == 200) {
    Serial.printf("[art]   saved photo of \"%s\": %s; free %u\n", found.c_str(), info.c_str(), freeHeap());
    return O_SAVED;
  }
  if (status == -1) return O_STOPPED;
  if (status == 404) return writeMarker(key) ? O_NONE : O_NET_ERROR;
  Serial.printf("[art]   photo failed: HTTP %d after %lu ms\n", status, millis() - t0);
  return O_NET_ERROR;
}

// An artist without a photo (yet) shows the thumbnail of their first album
// with one, marked as a stand-in; a photo found later replaces it.
struct StandInStats {
  uint32_t made = 0, had = 0, noCover = 0;
};
StandInStats standIns;

void makeArtistStandIn(const ArtistRecord &ar, LibReader &albums) {
  const String artist(ar.name, strnlen(ar.name, sizeof(ar.name)));
  if (placeholder(artist)) return;
  if (store.read(artistKey(artist.c_str()), thumbPixels)) {
    standIns.had++;
    return;
  }
  AlbumRecord al;
  bool made = false;
  for (uint32_t i = 0; i < ar.albumCount && !stopRequested && !made; i++) {
    if (!albums.read(ar.firstAlbum + i, &al) || (al.flags & ALBUM_NONE)) continue;
    const String album(al.name, strnlen(al.name, sizeof(al.name)));
    if (store.read(albumKey(artist.c_str(), album.c_str()), thumbPixels)) {
      made = store.write(artistKey(artist.c_str()), thumbPixels, THUMB_STAND_IN);
    }
  }
  if (made) standIns.made++;
  else standIns.noCover++;
}

// True if the album already has a cover, or a recent "no cover" marker.
bool alreadyHandled(uint32_t key) {
  FILINFO info;
  if (f_stat(cardPath(keyPath(key, "jpg")).c_str(), &info) == FR_OK) return true;
  if (f_stat(cardPath(keyPath(key, "no")).c_str(), &info) != FR_OK) return false;
  if (info.fsize < MARKER_SIZE) return false;  // from an older search
  const time_t now = time(nullptr);
  if (now < 1700000000) return true;  // clock not set yet: can't tell its age
  struct tm t = {};
  t.tm_year = (info.fdate >> 9) + 80;
  t.tm_mon = ((info.fdate >> 5) & 15) - 1;
  t.tm_mday = info.fdate & 31;
  return difftime(now, mktime(&t)) < MISSING_RETRY_DAYS * 86400.0;
}

// True if the album (or artist) has its own picture. Makes sure its
// thumbnail is in the store (no WiFi needed): pictures saved before the
// store had a "<key>.thm" file, which is moved in, or none.
bool makeMissingThumb(uint32_t key) {
  uint8_t flags = 0;
  if (store.read(key, thumbPixels, &flags) && !(flags & THUMB_STAND_IN)) return true;
  FILINFO info;
  const String jpg = keyPath(key, "jpg");
  if (f_stat(cardPath(jpg).c_str(), &info) != FR_OK) return false;
  const String thm = artThumbPath(jpg);
  const uint32_t t0 = millis();
  const bool moved = artReadThumb(cardPath(thm), thumbPixels);
  const bool ok = (moved || artMakeThumb(cardPath(jpg), thumbPixels)) && store.write(key, thumbPixels);
  if (ok && moved) f_unlink(cardPath(thm).c_str());
  if (!moved) Serial.printf("[art] thumbnail %s: %s, %lu ms\n", jpg.c_str(), ok ? "made" : "failed", millis() - t0);
  return true;
}

// ---------------------------------------------------------------- key migration
// Pictures saved before names were normalized are under legacy keys. Once
// per library generation (no WiFi), move them: albums and artists of the
// library (thumbnail, cover/photo and "no picture" marker), then the
// thumbnails songs look up for their own album (a "none" song's album isn't
// in the library any more, but its cover may be on the card).
uint32_t migratedGen = LIB_NONE;

bool moveFile(uint32_t from, uint32_t to, const char *ext) {
  FILINFO info;
  if (f_stat(cardPath(keyPath(from, ext)).c_str(), &info) != FR_OK) return false;
  char bucket[40];
  snprintf(bucket, sizeof(bucket), ART_DIR "/%02x", static_cast<unsigned>(to >> 24));
  f_mkdir(cardPath(bucket).c_str());
  f_unlink(cardPath(keyPath(to, ext)).c_str());
  return f_rename(cardPath(keyPath(from, ext)).c_str(), cardPath(keyPath(to, ext)).c_str()) == FR_OK;
}

// True if something was moved.
bool migrateKey(uint32_t legacy, uint32_t key, bool files) {
  if (legacy == key || store.read(key, thumbPixels)) return false;
  uint8_t flags = 0;
  const bool thumb = store.read(legacy, thumbPixels, &flags) && store.write(key, thumbPixels, flags);
  bool moved = false;
  if (files) {
    moved = moveFile(legacy, key, "jpg");
    moved = moveFile(legacy, key, "no") || moved;
  }
  return thumb || moved;
}

bool migrateKeys(LibReader &albums, LibReader &artists) {
  const uint32_t t0 = millis();
  uint32_t moved = 0;
  AlbumRecord al;
  ArtistRecord ar;
  for (uint32_t i = 0; i < artists.count() && !stopRequested; i++) {
    vTaskDelay(1);
    if (!artists.read(i, &ar)) continue;
    moved += migrateKey(legacyArtistKey(ar.name), artistKey(ar.name), true);
  }
  for (uint32_t i = 0; i < albums.count() && !stopRequested; i++) {
    vTaskDelay(1);
    if (!albums.read(i, &al) || (al.flags & (ALBUM_NONE | ALBUM_NONE_ALL)) || !artists.read(al.artistId, &ar)) continue;
    moved += migrateKey(legacyAlbumKey(ar.name, al.name), albumKey(ar.name, al.name), true);
  }
  LibReader order, songs;
  if (order.open(passGen, LF_ARTIST_INDEX, sizeof(uint32_t)) && songs.open(passGen, LF_SONGS, sizeof(SongRecord))) {
    order.setReadAhead(4096);
    uint32_t id, last = 0;
    SongRecord r;
    for (uint32_t pos = 0; pos < order.count() && !stopRequested; pos++) {
      if (pos % 16 == 0) vTaskDelay(1);
      if (!order.read(pos, &id) || !songs.read(id, &r) || !r.album[0]) continue;
      const char *artist = r.albumArtist[0] ? r.albumArtist : r.artist[0] ? r.artist : "Unknown Artist";
      const uint32_t key = albumKey(artist, r.album);
      if (key == last) continue;  // an album's songs are together
      last = key;
      if (migrateKey(legacyAlbumKey(artist, r.album), key, false) ||
          migrateKey(legacyAlbumKey("Various Artists", r.album), albumKey("Various Artists", r.album), false)) {
        moved++;
      }
    }
  }
  if (stopRequested) return false;
  Serial.printf("[art] moved %lu pictures to normalized keys in %lu ms\n", moved, millis() - t0);
  return true;
}


// ---------------------------------------------------------------- lyrics
// LRCLIB's /api/get: artist, title and length (within 2 s) pick a song; 404
// if it has none. The answer is one flat JSON object with the lyrics twice
// (plain and timed) and more: up to ~35 KB, read as it streams in, keeping
// the timed text if there is any, else the plain one. The text goes
// straight into the store's file (no room for a 12 KB buffer next to TLS):
// timed text, which comes after the plain one, is written over it.
class LyricsReader {
 public:
  uint32_t base = 0;     // where the text starts in the store
  uint16_t len = 0;      // its length once finish()ed
  bool synced = false, instrumental = false, writeFailed = false;

  void reset() {
    base = LyricStore::textStart();
    restart();
    writeFailed = false;
    synced = instrumental = false;
    depth = 0;
    inString = escaped = afterColon = false;
    target = T_NONE;
    key = "";
    scalar = "";
    uLeft = 0;
  }
  void feed(const uint8_t *d, int n) {
    for (int i = 0; i < n; i++) step(static_cast<char>(d[i]));
  }
  // The kept text, without trailing blank lines; cut after its last whole
  // line if it was too long.
  void finish() {
    flush();
    len = full ? contentAtNewline : content;
  }

 private:
  enum Target { T_NONE, T_KEY, T_PLAIN, T_SYNCED, T_SKIP };
  int depth = 0;
  bool inString = false, escaped = false, afterColon = false, full = false;
  Target target = T_NONE;
  String key, scalar;
  char stage[256];
  uint16_t staged = 0, written = 0, content = 0, contentAtNewline = 0;

  void restart() {  // a text starts (again) at base
    len = staged = written = content = contentAtNewline = 0;
    full = false;
  }
  void flush() {
    if (staged && !writeFailed) writeFailed = !LyricStore::writeText(base + written, stage, staged);
    written += staged;
    staged = 0;
  }
  uint32_t uValue = 0, uHigh = 0;
  uint8_t uLeft = 0;

  void put(char c) {
    if (target == T_KEY) {
      if (key.length() < 24) key += c;
      return;
    }
    if (target != T_PLAIN && target != T_SYNCED) return;
    if (c == '\r' || full) return;
    if (len >= LYRICS_MAX) {
      full = true;
      return;
    }
    stage[staged++] = c;
    len++;
    if (c == '\n') contentAtNewline = content;
    else if (c != ' ') content = len;
    if (staged == sizeof(stage)) flush();
  }
  void putCodepoint(uint32_t cp) {
    String s;
    appendCodepoint(s, cp);
    for (size_t i = 0; i < s.length(); i++) put(s[i]);
  }
  void step(char c) {
    if (inString) {
      if (uLeft) {
        uValue = uValue << 4 | (isdigit(static_cast<unsigned char>(c)) ? c - '0' : (tolower(c) - 'a' + 10) & 15);
        if (--uLeft == 0) {
          if (uValue >= 0xD800 && uValue < 0xDC00) uHigh = uValue;
          else if (uValue >= 0xDC00 && uValue < 0xE000 && uHigh) putCodepoint(0x10000 + ((uHigh - 0xD800) << 10) + (uValue - 0xDC00)), uHigh = 0;
          else putCodepoint(uValue), uHigh = 0;
        }
      } else if (escaped) {
        escaped = false;
        switch (c) {
          case 'n': put('\n'); break;
          case 't': put(' '); break;
          case 'r': case 'b': case 'f': break;
          case 'u': uLeft = 4; uValue = 0; break;
          default: put(c); break;
        }
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
        if (target == T_KEY) afterColon = false;
        target = T_NONE;
      } else {
        put(c);
      }
      return;
    }
    switch (c) {
      case '{': case '[': depth++; scalar = ""; break;
      case '}': case ']': case ',':
        if (depth == 1 && afterColon && key == "instrumental") instrumental = scalar == "true";
        if (c != ',') depth--;
        afterColon = false;
        scalar = "";
        break;
      case ':': afterColon = true; scalar = ""; break;
      case '"':
        inString = true;
        if (depth != 1) target = T_SKIP;
        else if (!afterColon) target = T_KEY, key = "";
        else if (key == "syncedLyrics") target = T_SYNCED, restart(), synced = true;
        else if (key == "plainLyrics" && !synced) target = T_PLAIN, restart();
        else target = T_SKIP;
        break;
      default:
        if (!isspace(static_cast<unsigned char>(c)) && scalar.length() < 8) scalar += c;
        break;
    }
  }
};

LyricsReader *lyrics = nullptr;  // while a pass runs
esp_http_client_handle_t lrcClient = nullptr;  // kept between songs: one TLS handshake per pass

uint32_t lrcRetryAfterS = 0;  // from a 429's Retry-After

esp_err_t onLrcData(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && strcasecmp(e->header_key, "Retry-After") == 0) {
    lrcRetryAfterS = strtoul(e->header_value, nullptr, 10);
  }
  if (e->event_id == HTTP_EVENT_ON_DATA && lyrics && esp_http_client_get_status_code(e->client) == 200) {
    lyrics->feed(static_cast<const uint8_t *>(e->data), e->data_len);
  }
  return ESP_OK;
}

void lrcClose() {
  if (lrcClient) esp_http_client_cleanup(lrcClient);
  lrcClient = nullptr;
}

// The HTTP status (0: connection error).
int lrcGet(const String &url) {
  if (!lrcClient) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = ART_USER_AGENT;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 1536;
    cfg.buffer_size_tx = 1024;
    cfg.event_handler = onLrcData;
    lrcClient = esp_http_client_init(&cfg);
    if (!lrcClient) return 0;
  } else {
    esp_http_client_set_url(lrcClient, url.c_str());
  }
  lyrics->reset();
  lrcRetryAfterS = 0;
  if (logTiming) Serial.printf("[art]   free %u, largest %u\n", freeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  const uint32_t t0 = millis();
  const esp_err_t err = esp_http_client_perform(lrcClient);
  const int status = err == ESP_OK ? esp_http_client_get_status_code(lrcClient) : 0;
  if (logTiming) Serial.printf("[art]   lrclib %d in %lu ms\n", status, millis() - t0);
  if (!status) lrcClose();  // a fresh connection next time
  return status;
}

uint16_t today() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? now / 86400 : 0;
}

// True if the song needs no lookup: known, or recently not found.
bool lyricsHandled(uint32_t key) {
  uint16_t day = 0;
  const LyricKind kind = LyricStore::writerFind(key, &day);
  if (kind == LYR_NOT_LOOKED) return false;
  if (kind != LYR_NONE) return true;
  const uint16_t now = today();
  return !now || now - day < MISSING_RETRY_DAYS;  // no clock yet: can't tell its age
}

bool songNames(const SongRecord &rec, String &artist, String &title) {
  title = String(rec.title, strnlen(rec.title, sizeof(rec.title)));
  artist = String(rec.artist, strnlen(rec.artist, sizeof(rec.artist)));
  if (placeholder(artist)) artist = String(rec.albumArtist, strnlen(rec.albumArtist, sizeof(rec.albumArtist)));
  // Untagged songs show their file name as the title: nothing to look up.
  return !placeholder(artist) && !placeholder(title) && !artist.equalsIgnoreCase("Various Artists");
}

uint32_t lastLrcRequest = 0;
uint32_t lrcInterval = LRC_INTERVAL_MS;

Outcome fetchLyrics(uint32_t key, const String &artist, const String &title, uint32_t durationMs) {
  String url = "https://lrclib.net/api/get?artist_name=" + urlEncode(artist) + "&track_name=" + urlEncode(title);
  if (durationMs >= 1000) url += "&duration=" + String((durationMs + 500) / 1000);
  int status = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    const uint32_t since = millis() - lastLrcRequest;
    if (since < lrcInterval && !nap(lrcInterval - since)) return O_STOPPED;
    lastLrcRequest = millis();
    status = lrcGet(url);
    if (stopRequested) return O_STOPPED;
    if (status == 429) {  // too fast: slow down, then this song again
      lrcInterval = min<uint32_t>(lrcInterval + 500, LRC_INTERVAL_MAX_MS);
      if (!nap(lrcRetryAfterS ? min<uint32_t>(lrcRetryAfterS, 60) * 1000 : 2000)) return O_STOPPED;
    } else if (status == 0) {
      // LRCLIB drops a kept connection now and then; a new handshake right
      // away often finds the heap still fragmented by the old one.
      if (!nap(1500)) return O_STOPPED;
    } else {
      break;
    }
  }
  if (status == 404) {
    return LyricStore::put(key, LYR_NONE, 0, 0, today()) ? O_NONE : O_NET_ERROR;
  }
  if (status != 200) return O_NET_ERROR;
  lyrics->finish();
  if (lyrics->writeFailed) return O_NET_ERROR;
  const LyricKind kind = lyrics->len ? (lyrics->synced ? LYR_SYNCED : LYR_PLAIN)
                                     : lyrics->instrumental ? LYR_INSTRUMENTAL : LYR_NONE;
  if (!LyricStore::put(key, kind, lyrics->base, lyrics->len, today())) return O_NET_ERROR;
  Serial.printf("[art]   lyrics: %s, %u bytes\n",
                kind == LYR_SYNCED ? "timed" : kind == LYR_PLAIN ? "plain" : kind == LYR_INSTRUMENTAL ? "instrumental" : "none",
                lyrics->len);
  if (kind == LYR_SYNCED || kind == LYR_PLAIN || kind == LYR_INSTRUMENTAL) {
    lyricsSaved++;
    return O_SAVED;
  }
  return O_NONE;
}

bool lyricsOn() {
  return uiStyle.lyrics;
}

// Takes the next wanted song (false: none).
bool takeWanted(uint32_t &id) {
  bool got = false;
  portENTER_CRITICAL(&wantLock);
  if (wantCount && wantGen == passGen) {
    id = wanted[--wantCount];
    got = true;
  } else {
    wantCount = 0;
  }
  portEXIT_CRITICAL(&wantLock);
  return got;
}

// ---------------------------------------------------------------- the pass
enum PassResult { P_DONE, P_STOPPED, P_NO_WIFI, P_NET, P_BUSY };

PassResult ensureWifi() {
  if (wifiUp) return P_DONE;
  if (freeHeap() < MIN_FREE_FOR_WIFI) {
    Serial.printf("[art] not enough memory for WiFi (%u free)\n", freeHeap());
    return P_BUSY;
  }
  if (!wifiStart()) return stopRequested ? P_STOPPED : P_NO_WIFI;
  return P_DONE;
}

void makeBucket(uint32_t key) {
  char bucket[40];
  snprintf(bucket, sizeof(bucket), ART_DIR "/%02x", static_cast<unsigned>(key >> 24));
  f_mkdir(cardPath(bucket).c_str());
}

// One song's lyrics: -1 if it needs no lookup, else the Outcome.
int lyricsFor(LibReader &songs, uint32_t id, uint32_t shown, uint32_t total, PassResult &wifi) {
  SongRecord rec;
  String artist, title;
  if (!songs.read(id, &rec) || !songNames(rec, artist, title)) return -1;
  const uint32_t key = LyricStore::key(artist, title);
  if (lyricsHandled(key)) return -1;
  wifi = ensureWifi();
  if (wifi != P_DONE) return O_STOPPED;
  Serial.printf("[art] %lu/%lu lyrics of \"%s\" by \"%s\"\n", shown, total, title.c_str(), artist.c_str());
  return fetchLyrics(key, artist, title, rec.durationMs);
}

// Four steps over one cursor: album covers; stand-ins for artists (an album
// cover, no WiFi); artist photos; songs' lyrics. Songs the player asked
// lyrics for go first.
PassResult runPass() {
  if (!audioReleaseEngine()) return P_BUSY;  // music started meanwhile
  // Lyrics steps and picture steps each have their files open only while
  // they run: a TLS handshake (LRCLIB, archive.org) needs every KB.
  const bool withLyrics = lyricsOn();
  LibReader *songs = nullptr;
  auto closeLyrics = [&]() {
    lrcClose();
    LyricStore::closeWriter();
    delete lyrics;
    lyrics = nullptr;
    delete songs;
    songs = nullptr;
  };
  auto openLyrics = [&]() {
    if (songs) return true;
    if (!LyricStore::exists() && !LyricStore::create(stopRequested)) return false;
    songs = new (std::nothrow) LibReader();
    lyrics = new (std::nothrow) LyricsReader();
    if (songs && lyrics && songs->open(passGen, LF_SONGS, sizeof(SongRecord)) && LyricStore::openWriter()) return true;
    closeLyrics();
    return false;
  };
  uint32_t netErrors = 0, saved = 0, none = 0;
  // Counts an outcome; a PassResult to end the pass with, or P_DONE to go on.
  auto tally = [&](int o) {
    switch (o) {
      case O_SAVED: saved++; netErrors = 0; break;
      case O_NONE: none++; netErrors = 0; break;
      case O_STOPPED: return P_STOPPED;
      case O_SKIPPED: break;
      case O_NET_ERROR:
        if (++netErrors >= 3) return P_NET;
        break;
    }
    return P_DONE;
  };
  auto wantedFirst = [&]() {
    uint32_t id;
    while (withLyrics && takeWanted(id)) {
      if (stopRequested) return P_STOPPED;
      if (!openLyrics()) return stopRequested ? P_STOPPED : P_BUSY;
      PassResult wifi = P_DONE;
      const int o = lyricsFor(*songs, id, 0, 0, wifi);
      if (wifi != P_DONE) return wifi;
      if (o >= 0) {
        const PassResult r = tally(o);
        if (r != P_DONE) return r;
      }
    }
    return P_DONE;
  };
  const PassResult first = wantedFirst();
  if (first != P_DONE) return first;
  closeLyrics();

  LibReader *albums = nullptr, *artists = nullptr;
  auto closeArt = [&]() {
    delete albums;
    delete artists;
    albums = artists = nullptr;
    store.close();
  };
  auto openArt = [&]() {
    albums = new (std::nothrow) LibReader();
    artists = new (std::nothrow) LibReader();
    if (!albums || !artists || !albums->open(passGen, LF_ALBUMS, sizeof(AlbumRecord)) ||
        !artists->open(passGen, LF_ARTISTS, sizeof(ArtistRecord))) {
      return P_BUSY;
    }
    albums->setReadAhead(4096);
    f_mkdir(cardPath(ART_DIR).c_str());
    if (!ThumbStore::exists() && !ThumbStore::create(stopRequested)) return stopRequested ? P_STOPPED : P_BUSY;
    if (migratedGen != passGen) {
      if (!migrateKeys(*albums, *artists)) return P_STOPPED;
      migratedGen = passGen;
    }
    return P_DONE;
  };
  struct Closer {
    std::function<void()> f;
    ~Closer() { f(); }
  } closer{[&]() { closeArt(); closeLyrics(); }};
  // Lyrics first (every song), then the pictures: album covers, artist
  // stand-ins, artist photos. Indexes into the files go by the catalog's
  // counts (the same generation).
  const uint32_t nSongs = withLyrics ? Catalog::songCount() : 0;
  const uint32_t nAlbums = Catalog::albumCount(), nArtists = Catalog::artistCount();
  if (cursorSongs != nSongs) {  // Lyrics turned on or off: the steps moved
    cursorSongs = nSongs;
    cursor = 0;
  }
  picturesFrom = nSongs;
  const uint32_t total = nSongs + nAlbums + 2 * nArtists;
  progressTotal = total;
  for (; cursor < total; cursor++) {
    progressDone = cursor;
    if (stopRequested) return P_STOPPED;
    // Also keeps steps without network waits from starving core 0's idle
    // task (watchdog).
    vTaskDelay(pdMS_TO_TICKS(CARD_PAUSE_MS));
    AlbumRecord al;
    ArtistRecord ar;
    Outcome o;
    if (cursor < nSongs) {
      if (!openLyrics()) return stopRequested ? P_STOPPED : P_BUSY;
      PassResult wifi = P_DONE;
      const int lo = lyricsFor(*songs, cursor, cursor + 1, total, wifi);
      if (wifi != P_DONE) return wifi;
      if (lo < 0) continue;
      o = static_cast<Outcome>(lo);
      const PassResult r = tally(o);
      if (r != P_DONE) return r;
      continue;
    }
    if (!albums) {
      if (songs) {
        Serial.printf("[art] lyrics done: %lu saved, %lu without; pictures next\n", saved, none);
        saved = none = 0;
      }
      closeLyrics();
      const PassResult r = openArt();
      if (r != P_DONE) return r;
    }
    const uint32_t c = cursor - nSongs;  // the pictures' own cursor
    if (c < nAlbums) {
      if (!albums->read(c, &al) || !al.name[0] || (al.flags & (ALBUM_NONE | ALBUM_NONE_ALL))) continue;
      const String album(al.name, strnlen(al.name, sizeof(al.name)));
      if (placeholder(album)) continue;  // songs without an album tag
      String artist;
      if (artists->read(al.artistId, &ar)) artist = String(ar.name, strnlen(ar.name, sizeof(ar.name)));
      // Without an artist, a search by album name alone matches too many
      // albums to trust ("Greatest Hits"): skip.
      if (placeholder(artist)) continue;
      const uint32_t key = albumKey(artist.c_str(), album.c_str());
      if (makeMissingThumb(key)) continue;
      if (alreadyHandled(key)) continue;
      albums->setReadAhead(0);  // 4 KB the TLS handshake needs (archive.org covers)
      const PassResult w = ensureWifi();
      if (w != P_DONE) return w;
      makeBucket(key);
      Serial.printf("[art] %lu/%lu \"%s\" by \"%s\"\n", cursor + 1, total, album.c_str(), artist.c_str());
      o = fetchAlbum(key, album, artist);
    } else if (c < nAlbums + nArtists) {
      if (c == nAlbums) standIns = StandInStats();
      if (artists->read(c - nAlbums, &ar)) makeArtistStandIn(ar, *albums);
      if (c + 1 == nAlbums + nArtists) {
        Serial.printf("[art] artist stand-ins: %lu made, %lu had a picture, %lu without an album cover\n",
                      standIns.made, standIns.had, standIns.noCover);
      }
      continue;
    } else {
      if (!artists->read(c - nAlbums - nArtists, &ar)) continue;
      const String artist(ar.name, strnlen(ar.name, sizeof(ar.name)));
      if (placeholder(artist) || artist.equalsIgnoreCase("Various Artists")) continue;
      const uint32_t key = artistKey(artist.c_str());
      if (makeMissingThumb(key)) continue;
      if (alreadyHandled(key)) continue;
      albums->setReadAhead(0);  // 4 KB the TLS handshake needs (archive.org covers)
      const PassResult w = ensureWifi();
      if (w != P_DONE) return w;
      makeBucket(key);
      Serial.printf("[art] %lu/%lu photo of \"%s\"\n", cursor + 1, total, artist.c_str());
      o = fetchArtist(key, artist);
    }
    const PassResult r = tally(o);
    if (r != P_DONE) return r;
  }
  closeArt();
  const PassResult last = wantedFirst();
  if (last != P_DONE) return last;
  progressDone = total;
  Serial.printf("[art] pass done: %lu saved, %lu without one\n", saved, none);
  return P_DONE;
}

void fetchTask(void *) {
  const uint32_t t0 = millis();
  const PassResult r = runPass();
  store.close();
  wifiStop();
  static const char *const names[] = {"done", "stopped", "no WiFi", "network errors", "busy"};
  Serial.printf("[art] pass ended (%s) after %lu s; free %u\n", names[r], (millis() - t0) / 1000, freeHeap());
  if (r == P_DONE && albumsSkipped) cursor = 0;  // albums still to do
  else if (r == P_DONE) finishedGen = passGen;
  if (r == P_DONE) albumsSkipped = false;
  if (r == P_NO_WIFI || r == P_NET) retryAt = millis() + RETRY_AFTER_MS;
  task = nullptr;
  vTaskDelete(nullptr);
}

bool startPass() {
  if (task || !enabled || !Catalog::isOpen() || libraryIndexing()) return false;
  String s, p;
  if (!wifiSavedCredentials(s, p)) return false;
  passGen = Catalog::generation();
  if (cursorGen != passGen) {
    cursorGen = passGen;
    cursor = 0;
  }
  stopRequested = false;
  progressDone = cursor;
  progressTotal = Catalog::albumCount() + 2 * Catalog::artistCount() + (lyricsOn() ? Catalog::songCount() : 0);
  picturesFrom = lyricsOn() ? Catalog::songCount() : 0;
  return xTaskCreatePinnedToCore(fetchTask, "artfetch", TASK_STACK, nullptr, 1, &task, 0) == pdPASS;
}

}  // namespace

uint32_t artAlbumKey(const char *albumArtist, const char *album) {
  if (!album || !album[0]) return 0;
  return albumKey(albumArtist ? albumArtist : "", album);
}

uint32_t artArtistKey(const char *artist) {
  if (!artist || placeholder(artist)) return 0;
  return artistKey(artist);
}

String artArtistPhotoPath(const char *artist) {
  if (!artist || placeholder(artist)) return "";
  return keyPath(artistKey(artist), "jpg");
}

String artCoverPath(const char *albumArtist, const char *album) {
  if (!album || !album[0]) return "";
  return keyPath(albumKey(albumArtist ? albumArtist : "", album), "jpg");
}

void artFetchInit() {
  audioSetBeforeRestore(artFetchStop);
}

bool artFetchEnabled() {
  return enabled;
}

void artFetchSetEnabled(bool on) {
  enabled = on;
}

void artFetchTick() {
  if (audioState() == PLAYING || libraryIndexing()) {
    idleSince = 0;
    return;
  }
  if (!idleSince) idleSince = millis() | 1;
  if (task || !enabled || millis() - idleSince < IDLE_BEFORE_MS) return;
  if (finishedGen == Catalog::generation() && !(wantCount && lyricsOn())) return;
  if (retryAt && static_cast<int32_t>(millis() - retryAt) < 0) return;
  retryAt = 0;
  startPass();
}

void artFetchNow() {
  if (task) {
    Serial.println("[art] already running");
    return;
  }
  finishedGen = LIB_NONE;
  retryAt = 0;
  if (!startPass()) Serial.println("[art] can't start (art off for this card, no WiFi saved, or library busy)");
}

void artFetchArtistsNow() {
  artFetchStop();
  cursorGen = Catalog::generation();
  cursor = (lyricsOn() ? Catalog::songCount() : 0) + Catalog::albumCount();
  cursorSongs = lyricsOn() ? Catalog::songCount() : 0;
  albumsSkipped = true;
  artFetchNow();
}

void artFetchStop() {
  if (!task) return;
  stopRequested = true;
  const uint32_t t0 = millis();
  while (task && millis() - t0 < 4 * HTTP_TIMEOUT_MS) delay(10);
  Serial.printf("[art] stopped in %lu ms\n", millis() - t0);
}

bool artFetchRunning() {
  return task != nullptr;
}

String artFetchStatus() {
  if (!task) return "";
  const uint32_t total = progressTotal, done = progressDone, from = picturesFrom;
  if (done < from) return "Lyrics\n" + String(done * 100 / from) + "%";
  return "Art\n" + String(total > from ? (done - from) * 100 / (total - from) : 0) + "%";
}

void artLyricsWant(uint32_t songId) {
  if (!Catalog::isOpen()) return;
  portENTER_CRITICAL(&wantLock);
  if (wantGen != Catalog::generation()) wantCount = 0, wantGen = Catalog::generation();
  bool there = false;
  for (int i = 0; i < wantCount; i++) there |= wanted[i] == songId;
  if (!there) {
    if (wantCount == WANT_MAX) memmove(wanted, wanted + 1, --wantCount * sizeof(wanted[0]));
    wanted[wantCount++] = songId;  // the newest is taken first
  }
  portEXIT_CRITICAL(&wantLock);
}

uint32_t artLyricsSaved() {
  return lyricsSaved;
}
