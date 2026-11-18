#include "ArtFetch.h"

#include <atomic>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <time.h>

#include "ArtScan.h"
#include "AudioPlayer.h"
#include "CardFile.h"
#include "Catalog.h"
#include "Config.h"
#include "LibFormat.h"
#include "LibStore.h"
#include "Library.h"
#include "WifiSetup.h"

namespace {

#define ART_DIR LIB_DIR "/art"
#define IDLE_BEFORE_MS 10000         // nothing playing this long: start a pass
#define RETRY_AFTER_MS (10 * 60000)  // after a network failure
#define MISSING_RETRY_DAYS 30
#define MB_INTERVAL_MS 1100          // MusicBrainz allows one request per second
#define JSON_BYTES 1536              // start of a search answer: the first match
#define MIN_FREE_FOR_WIFI 90000      // WiFi ~43 KB + TLS ~45 KB
#define HTTP_TIMEOUT_MS 5000         // also bounds how long a stop request waits
#define TASK_STACK 8192              // TLS handshake runs on it

enum Outcome { O_SAVED, O_NONE, O_NET_ERROR, O_STOPPED };

bool enabled = false;
TaskHandle_t task = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<uint32_t> progressDone{0}, progressTotal{0};
uint32_t idleSince = 0;
uint32_t retryAt = 0;
uint32_t cursor = 0;              // next album to look at (survives stopped passes)
uint32_t cursorGen = LIB_NONE;    // generation the cursor belongs to
uint32_t finishedGen = LIB_NONE;  // every album of this generation was handled
uint32_t lastMbRequest = 0;
uint32_t passGen = LIB_NONE;

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
uint32_t albumKey(const char *artist, const char *album) {
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

String jsonString(const char *from, const char *key) {
  const String k = String("\"") + key + "\":\"";
  const char *p = strstr(from, k.c_str());
  if (!p) return "";
  p += k.length();
  const char *end = strchr(p, '"');
  return end ? String(p).substring(0, end - p) : String();
}

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
bool writeMarker(uint32_t key) {
  FIL *f = cardNewFil();
  bool ok = f && f_open(f, cardPath(keyPath(key, "no")).c_str(), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) f_close(f);  // empty: its date is what counts
  cardDeleteFil(f);
  return ok;
}

Outcome fetchAlbum(uint32_t key, const String &album, const String &artist) {
  // 1. Search (plain HTTP: no TLS needed).
  if (!nap(MB_INTERVAL_MS - min<uint32_t>(MB_INTERVAL_MS, millis() - lastMbRequest))) return O_STOPPED;
  String query = "releasegroup:" + phrase(searchName(album));
  query += " AND artist:" + phrase(artist);
  const String url = "http://musicbrainz.org/ws/2/release-group/?fmt=json&limit=1&query=" + urlEncode(query);
  char *json = static_cast<char *>(malloc(JSON_BYTES));
  if (!json) return O_NET_ERROR;
  size_t len = 0;
  int status = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    lastMbRequest = millis();
    len = 0;
    json[0] = 0;
    status = httpGet(url, [&](const uint8_t *d, int n) {
      const size_t take = min<size_t>(n, JSON_BYTES - 1 - len);
      memcpy(json + len, d, take);
      len += take;
      json[len] = 0;
      return true;  // read the rest so the connection ends cleanly
    });
    if (status != 503) break;  // busy: MusicBrainz asks clients to slow down
    if (!nap(3000 * (attempt + 1))) break;
  }
  const char *groups = status == 200 ? strstr(json, "\"release-groups\":[{") : nullptr;
  const String id = groups ? jsonString(groups, "id") : String();
  const String title = groups ? jsonString(groups, "title") : String();
  const char *sc = groups ? strstr(groups, "\"score\":") : nullptr;
  const int score = sc ? atoi(sc + 8) : 0;
  free(json);
  if (status == -1) return O_STOPPED;
  if (status != 200) {
    Serial.printf("[art]   search failed: HTTP %d\n", status);
    return O_NET_ERROR;
  }
  if (id.length() != 36 || score < 90) {
    Serial.printf("[art]   no confident match (best \"%s\", score %d)\n", title.c_str(), score);
    return writeMarker(key) ? O_NONE : O_NET_ERROR;
  }

  // 2. Cover: coverartarchive.org redirects to archive.org, which needs HTTPS.
  const String tmp = keyPath(key, "tmp");
  FIL *f = cardNewFil();
  if (!f || f_open(f, cardPath(tmp).c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    cardDeleteFil(f);
    return O_NET_ERROR;
  }
  uint32_t bytes = 0;
  bool writeOk = true;
  const uint32_t t0 = millis();
  status = httpGet("http://coverartarchive.org/release-group/" + id + "/front-250", [&](const uint8_t *d, int n) {
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
    if (f_rename(cardPath(tmp).c_str(), cardPath(jpg).c_str()) != FR_OK) return O_NET_ERROR;
    CardFile cf;
    uint16_t w = 0, h = 0;
    const char *kind = cf.open(jpg) ? artDescribeImage(cf, 0, cf.size(), w, h) : "?";
    Serial.printf("[art]   saved \"%s\": %lu bytes, %s %ux%u, %lu ms; free %u\n", title.c_str(), bytes, kind, w, h,
                  millis() - t0, freeHeap());
    return O_SAVED;
  }
  f_unlink(cardPath(tmp).c_str());
  if (status == -1) return O_STOPPED;
  if (status == 404) {  // the release group has no front cover
    Serial.printf("[art]   \"%s\" has no cover\n", title.c_str());
    return writeMarker(key) ? O_NONE : O_NET_ERROR;
  }
  Serial.printf("[art]   cover failed: HTTP %d after %lu ms; free %u\n", status, millis() - t0, freeHeap());
  return O_NET_ERROR;
}

// True if the album already has a cover, or a recent "no cover" marker.
bool alreadyHandled(uint32_t key) {
  FILINFO info;
  if (f_stat(cardPath(keyPath(key, "jpg")).c_str(), &info) == FR_OK) return true;
  if (f_stat(cardPath(keyPath(key, "no")).c_str(), &info) != FR_OK) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return true;  // clock not set yet: can't tell its age
  struct tm t = {};
  t.tm_year = (info.fdate >> 9) + 80;
  t.tm_mon = ((info.fdate >> 5) & 15) - 1;
  t.tm_mday = info.fdate & 31;
  return difftime(now, mktime(&t)) < MISSING_RETRY_DAYS * 86400.0;
}

// ---------------------------------------------------------------- the pass
enum PassResult { P_DONE, P_STOPPED, P_NO_WIFI, P_NET, P_BUSY };

PassResult runPass() {
  if (!audioReleaseEngine()) return P_BUSY;  // music started meanwhile
  LibReader albums, artists;
  if (!albums.open(passGen, LF_ALBUMS, sizeof(AlbumRecord)) ||
      !artists.open(passGen, LF_ARTISTS, sizeof(ArtistRecord))) {
    return P_BUSY;
  }
  albums.setReadAhead(4096);
  f_mkdir(cardPath(ART_DIR).c_str());
  const uint32_t total = albums.count();
  progressTotal = total;
  uint32_t netErrors = 0, saved = 0, none = 0;
  for (; cursor < total; cursor++) {
    progressDone = cursor;
    if (stopRequested) return P_STOPPED;
    AlbumRecord al;
    ArtistRecord ar;
    if (!albums.read(cursor, &al) || !al.name[0]) continue;
    const String album(al.name, strnlen(al.name, sizeof(al.name)));
    if (placeholder(album)) continue;  // songs without an album tag
    String artist;
    if (artists.read(al.artistId, &ar)) artist = String(ar.name, strnlen(ar.name, sizeof(ar.name)));
    // Without an artist, a search by album name alone matches too many
    // albums to trust ("Greatest Hits"): skip.
    if (placeholder(artist)) continue;
    const uint32_t key = albumKey(artist.c_str(), album.c_str());
    if (alreadyHandled(key)) continue;
    if (!wifiUp) {
      if (freeHeap() < MIN_FREE_FOR_WIFI) {
        Serial.printf("[art] not enough memory for WiFi (%u free)\n", freeHeap());
        return P_BUSY;
      }
      if (!wifiStart()) return stopRequested ? P_STOPPED : P_NO_WIFI;
    }
    char bucket[40];
    snprintf(bucket, sizeof(bucket), ART_DIR "/%02x", static_cast<unsigned>(key >> 24));
    f_mkdir(cardPath(bucket).c_str());
    Serial.printf("[art] %lu/%lu \"%s\" by \"%s\"\n", cursor + 1, total, album.c_str(), artist.c_str());
    switch (fetchAlbum(key, album, artist)) {
      case O_SAVED: saved++; netErrors = 0; break;
      case O_NONE: none++; netErrors = 0; break;
      case O_STOPPED: return P_STOPPED;
      case O_NET_ERROR:
        if (++netErrors >= 3) return P_NET;
        break;
    }
  }
  progressDone = total;
  Serial.printf("[art] pass done: %lu covers saved, %lu without one\n", saved, none);
  return P_DONE;
}

void fetchTask(void *) {
  const uint32_t t0 = millis();
  const PassResult r = runPass();
  wifiStop();
  static const char *const names[] = {"done", "stopped", "no WiFi", "network errors", "busy"};
  Serial.printf("[art] pass ended (%s) after %lu s; free %u\n", names[r], (millis() - t0) / 1000, freeHeap());
  if (r == P_DONE) finishedGen = passGen;
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
  progressTotal = Catalog::albumCount();
  return xTaskCreatePinnedToCore(fetchTask, "artfetch", TASK_STACK, nullptr, 1, &task, 0) == pdPASS;
}

}  // namespace

String artCoverPath(const char *albumArtist, const char *album) {
  if (!album || !album[0]) return "";
  return keyPath(albumKey(albumArtist ? albumArtist : "", album), "jpg");
}

void artFetchInit() {
  audioSetBeforeRestore(artFetchStop);
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
  if (finishedGen == Catalog::generation()) return;
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
  const uint32_t total = progressTotal;
  return "Art\n" + String(total ? progressDone * 100 / total : 0) + "%";
}
