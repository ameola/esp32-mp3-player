#include "LyricStore.h"

#include "LibFormat.h"
#include "Text.h"

namespace {

#define STORE_PATH LIB_DIR "/lyrics.dat"
#define STORE_TMP LIB_DIR "/lyrics.tmp"
#define LYR_SLOTS 16384  // 2x the songs of a large library
#define LYR_PROBES 24
#define INDEX_BYTES (LYR_SLOTS * sizeof(Slot))

struct __attribute__((packed)) Slot {
  uint32_t key;     // 0: empty
  uint32_t offset;  // text in the file
  uint16_t len;
  uint8_t kind;     // LyricKind
  uint8_t reserved;
  uint16_t day;     // days since 1970 it was looked up
  uint16_t reserved2;
};
static_assert(sizeof(Slot) == 16, "Slot size");

FIL *writer = nullptr;

// The slot holding `key`, or the first empty one on its probe path (key 0
// in `out`); -1 if neither (index full there, or a read error).
int32_t locate(FIL *f, uint32_t key, Slot &out) {
  for (uint32_t i = 0; i < LYR_PROBES; i++) {
    const uint32_t s = (key + i * 2654435761u) % LYR_SLOTS;
    UINT got = 0;
    if (f_lseek(f, static_cast<FSIZE_t>(s) * sizeof(Slot)) != FR_OK || f_read(f, &out, sizeof(out), &got) != FR_OK ||
        got != sizeof(out)) {
      return -1;
    }
    if (out.key == key || out.key == 0) return s;
  }
  return -1;
}

String readText(FIL *f, const Slot &slot) {
  String text;
  char *buf = static_cast<char *>(malloc(slot.len + 1));
  UINT got = 0;
  if (buf && f_lseek(f, slot.offset) == FR_OK && f_read(f, buf, slot.len, &got) == FR_OK && got == slot.len) {
    buf[got] = 0;
    text = buf;
  } else {
    Serial.printf("[lyrics] text not read (%s)\n", buf ? "card" : "no memory");
  }
  free(buf);
  return text;
}

}  // namespace

namespace LyricStore {

uint32_t key(const String &artist, const String &title) {
  String s = foldAscii(artist.c_str()) + "\n" + foldAscii(title.c_str());
  s.toLowerCase();
  uint32_t h = 2166136261u;  // FNV-1a
  for (size_t i = 0; i < s.length(); i++) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
  return h ? h : 1;
}

LyricKind find(uint32_t k, String *text, uint16_t *day) {
  FIL *f = cardNewFil();
  if (!f) return LYR_NOT_LOOKED;
  LyricKind kind = LYR_NOT_LOOKED;
  if (f_open(f, cardPath(STORE_PATH).c_str(), FA_READ) == FR_OK) {
    Slot slot;
    if (locate(f, k, slot) >= 0 && slot.key == k) {
      kind = static_cast<LyricKind>(slot.kind);
      if (day) *day = slot.day;
      if (text && (kind == LYR_SYNCED || kind == LYR_PLAIN)) *text = readText(f, slot);
    }
    f_close(f);
  }
  cardDeleteFil(f);
  return kind;
}

bool exists() {
  FILINFO info;
  return f_stat(cardPath(STORE_PATH).c_str(), &info) == FR_OK && info.fsize >= INDEX_BYTES;
}

bool create(const std::atomic<bool> &stop) {
  const uint32_t t0 = millis();
  f_unlink(cardPath(STORE_TMP).c_str());
  FIL *f = cardNewFil();
  uint8_t *zeros = static_cast<uint8_t *>(calloc(1, 512));
  bool ok = f && zeros && f_open(f, cardPath(STORE_TMP).c_str(), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) {
    for (uint32_t i = 0; ok && i < INDEX_BYTES / 512; i++) {
      UINT w = 0;
      ok = !stop && cardWrite(f, zeros, 512, &w) == FR_OK && w == 512;
      if (i % 16 == 15) vTaskDelay(1);
    }
    ok = f_close(f) == FR_OK && ok;
    if (ok) {
      f_unlink(cardPath(STORE_PATH).c_str());
      ok = f_rename(cardPath(STORE_TMP).c_str(), cardPath(STORE_PATH).c_str()) == FR_OK;
    }
    if (!ok) f_unlink(cardPath(STORE_TMP).c_str());
  }
  free(zeros);
  cardDeleteFil(f);
  Serial.printf("[lyrics] store %s in %lu ms\n", ok ? "created" : "not created", millis() - t0);
  return ok;
}

bool openWriter() {
  if (writer) return true;
  writer = cardNewFil();
  if (writer && f_open(writer, cardPath(STORE_PATH).c_str(), FA_READ | FA_WRITE) == FR_OK) return true;
  cardDeleteFil(writer);
  writer = nullptr;
  return false;
}

void closeWriter() {
  if (!writer) return;
  f_close(writer);
  cardDeleteFil(writer);
  writer = nullptr;
}

LyricKind writerFind(uint32_t k, uint16_t *day) {
  Slot slot;
  if (!writer || locate(writer, k, slot) < 0 || slot.key != k) return LYR_NOT_LOOKED;
  if (day) *day = slot.day;
  return static_cast<LyricKind>(slot.kind);
}

uint32_t textStart() {
  return writer ? f_size(writer) : 0;
}

bool writeText(uint32_t offset, const char *data, uint16_t len) {
  UINT w = 0;
  return writer && f_lseek(writer, offset) == FR_OK && cardWrite(writer, data, len, &w) == FR_OK && w == len;
}

bool put(uint32_t k, LyricKind kind, uint32_t textOffset, uint16_t textLen, uint16_t day) {
  Slot slot;
  const int32_t s = writer ? locate(writer, k, slot) : -1;
  if (s < 0) return false;
  slot = {k, textLen ? textOffset : 0, textLen, static_cast<uint8_t>(kind), 0, day, 0};
  UINT w = 0;
  // The text is on the card already: a reader never sees a slot pointing past it.
  if (f_lseek(writer, static_cast<FSIZE_t>(s) * sizeof(Slot)) != FR_OK ||
      cardWrite(writer, &slot, sizeof(slot), &w) != FR_OK || w != sizeof(slot)) {
    return false;
  }
  return f_sync(writer) == FR_OK;
}

}  // namespace LyricStore
