#pragma once
// All list thumbnails (album covers, artist photos and stand-ins) in one
// file, LIB_DIR "/art/thumbs.dat", kept open while used.
//
// Reading a thumbnail from its own small file meant a FatFs path lookup:
// the root folder (300 entries here), then .mp3player, art (256 buckets)
// and the bucket: 30-70 ms each, too slow for a list that moves. In one
// open file it's a seek (SeekIndex: no cluster chain walk from the start)
// and one 2 KB read: a few ms.
//
// The file has STORE_SLOTS slots of 2 KB (16 MB), written as zeros once.
// A thumbnail goes in the slot its key hashes to, or one of the next few
// (linear probing). A slot holds a magic number, the key, flags and a CRC of
// the pixels; anything else (zeros) is an empty slot.
#include <Arduino.h>
#include <atomic>

#include "ArtView.h"
#include "CardFile.h"

// A stand-in: an artist's thumbnail made from one of their album covers,
// replaced when a photo is found.
#define THUMB_STAND_IN 1

class ThumbStore {
 public:
  ~ThumbStore() { close(); }
  static bool exists();
  // Writes the empty store (~16 MB, a few seconds to a minute depending on
  // the card); stops early, leaving no file, if `stop` turns true.
  static bool create(const std::atomic<bool> &stop);

  // False when there's none (or no store yet: after a failed open, read()
  // doesn't try again until retry()).
  bool read(uint32_t key, uint16_t *pixels, uint8_t *flags = nullptr);
  bool write(uint32_t key, const uint16_t *pixels, uint8_t flags = 0);
  void retry() { openFailed_ = false; }
  void close();

 private:
  bool open(bool writable);
  bool readSlot(uint32_t slot);
  FIL *fil_ = nullptr;
  bool writable_ = false, openFailed_ = false;
  SeekIndex seek_;
  uint8_t *buf_ = nullptr;  // one slot
};
