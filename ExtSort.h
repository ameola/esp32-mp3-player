#pragma once
// External merge sort of fixed 48-byte records (44-byte key + 32-bit id),
// so sorting cost in RAM stays fixed however large the library is.
//
// add() fills a RAM buffer; each full buffer is sorted and written to a temp
// file as a "run". next() merges all runs at once with a heap, refilling a
// slice of the same buffer per run. If everything fits in the buffer, no file
// is used. Order: memcmp of key, then id.

#include <Arduino.h>
#include <vector>
#include "ff.h"

#include "CardFile.h"

struct SortRec {
  uint8_t key[44];
  uint32_t id;
};
static_assert(sizeof(SortRec) == 48, "SortRec size");

// Sort order: memcmp of key, then id.
inline bool sortRecLess(const SortRec &a, const SortRec &b) {
  const int c = memcmp(a.key, b.key, sizeof(a.key));
  return c != 0 ? c < 0 : a.id < b.id;
}

// A source of sort records in order.
class KeyStream {
public:
  virtual ~KeyStream() = default;
  virtual bool next(SortRec &out) = 0;  // false when done (or on error)
};

class ExtSort : public KeyStream {
public:
  ~ExtSort() { end(); }
  // tmpPath: Arduino-style path for the run file. bufferBytes: RAM to use.
  // expectedRecords (if known): reserves the run table up front.
  bool begin(const String &tmpPath, size_t bufferBytes, uint32_t expectedRecords = 0);
  bool add(const SortRec &rec);
  bool finish();               // call once after the last add()
  bool next(SortRec &out) override;  // false when done
  void end();                  // frees RAM, deletes the temp file
  uint32_t count() const { return total_; }
  uint32_t runCount() const { return runs_.size(); }
  const String &error() const { return error_; }  // why it failed, if it did

private:
  struct Run {
    uint32_t start;     // first record index in the temp file
    uint32_t count;
    uint32_t loaded;    // records read into the buffer so far
    uint32_t slice;     // first buffer slot of this run's slice
    uint16_t bufLen;
    uint16_t bufPos;
  };
  bool spill();
  bool refill(uint32_t r);
  bool less(uint32_t a, uint32_t b) const;  // compares runs' current records
  void siftDown(size_t i);

  SortRec *buf_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t n_ = 0;
  uint32_t total_ = 0;
  uint32_t pos_ = 0;       // in-memory mode read position
  bool spilled_ = false;
  bool ok_ = false;
  FIL *fil_ = nullptr;
  SeekIndex seek_;  // merge refills jump between runs
  String path_;
  std::vector<Run> runs_;
  std::vector<uint32_t> heap_;
  String error_;
  uint32_t sliceLen_ = 0;
};
