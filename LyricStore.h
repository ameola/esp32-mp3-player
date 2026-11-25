#pragma once
// Downloaded lyrics (LRCLIB), all in one card file, LIB_DIR "/lyrics.dat",
// so thousands of songs don't each take a cluster: a fixed index of
// LYR_SLOTS 16-byte slots (key, where the text is, what was found, when),
// then the texts, appended. A song's key is a hash of its artist and title
// (folded to ASCII, lower case), so copies of a song share it and it
// survives library rebuilds.
//
// One writer (the download pass, which keeps the file open) and readers that
// open it for one lookup each (a FIL caches a sector and the file size, so a
// long-open reader wouldn't see new entries).
#include <Arduino.h>

#include <atomic>

#include "CardFile.h"

enum LyricKind : uint8_t {
  LYR_NOT_LOOKED = 0,  // empty slot
  LYR_SYNCED,          // timed .lrc text
  LYR_PLAIN,           // untimed text
  LYR_INSTRUMENTAL,
  LYR_NONE,            // not found; looked up on `day`
};

namespace LyricStore {

uint32_t key(const String &artist, const String &title);  // never 0

// Reader: what's known about a song; for LYR_SYNCED / LYR_PLAIN, the text.
LyricKind find(uint32_t key, String *text, uint16_t *day = nullptr);

// Writer (the pass). create() makes the empty index once.
bool exists();
bool create(const std::atomic<bool> &stop);
bool openWriter();
void closeWriter();
// Looks a key up through the writer's handle.
LyricKind writerFind(uint32_t key, uint16_t *day);
// A text is streamed in first: written at textStart() (the file's end) and
// on, overwritten as needed; then put() records it. `day` is days since 1970.
uint32_t textStart();
bool writeText(uint32_t offset, const char *data, uint16_t len);
bool put(uint32_t key, LyricKind kind, uint32_t textOffset, uint16_t textLen, uint16_t day);

}  // namespace LyricStore
