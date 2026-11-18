#include "ExtSort.h"
#include <new>

#include <algorithm>

#include "CardFile.h"

static bool recLess(const SortRec &a, const SortRec &b) {
  return sortRecLess(a, b);
}

bool ExtSort::begin(const String &tmpPath, size_t bufferBytes, uint32_t expectedRecords) {
  end();
  error_ = "";
  cap_ = bufferBytes / sizeof(SortRec);
  buf_ = static_cast<SortRec *>(malloc(cap_ * sizeof(SortRec)));
  path_ = tmpPath;
  ok_ = buf_ != nullptr && cap_ >= 16;
  if (ok_ && expectedRecords > 0) {
    // The run table and merge heap would otherwise grow by doubling while
    // records are fed in; by then reading songs has fragmented the heap and
    // a 5 KB block may not exist (seen with ~130 runs during playback).
    // Every spill but the last is a full buffer, so this is an upper bound.
    // The temp file's FIL (4.6 KB, with its sector buffer) likewise.
    const uint32_t runs = expectedRecords / cap_ + 2;
    try {
      runs_.reserve(runs);
      heap_.reserve(runs);
    } catch (const std::bad_alloc &) {
      end();
      ok_ = false;
    }
    if (ok_ && expectedRecords > cap_) {
      fil_ = cardNewFil();
      if (!fil_) {
        end();
        ok_ = false;
      }
    }
  }
  return ok_;
}

bool ExtSort::spill() {
  if (!fil_) {
    fil_ = cardNewFil();  // out of memory: fail the sort, don't abort
    if (!fil_) {
      error_ = "no memory for the temp file";
      return false;
    }
  }
  if (!spilled_) {
    const FRESULT r = f_open(fil_, cardPath(path_).c_str(), FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
    if (r != FR_OK) {
      error_ = "cannot create the temp file (FatFs " + String(r) + ")";
      return false;
    }
    spilled_ = true;
  }
  std::sort(buf_, buf_ + n_, recLess);
  const uint32_t start = runs_.empty() ? 0 : runs_.back().start + runs_.back().count;
  UINT written = 0;
  const UINT bytes = n_ * sizeof(SortRec);
  FRESULT r = f_lseek(fil_, static_cast<FSIZE_t>(start) * sizeof(SortRec));
  if (r == FR_OK) {
    r = cardWrite(fil_, buf_, bytes, &written);
  }
  if (r != FR_OK || written != bytes) {
    error_ = "temp file write failed (FatFs " + String(r) + ", " + String(written) + " of " +
             String(bytes) + " bytes)";
    return false;
  }
  runs_.push_back({start, n_, 0, 0, 0, 0});  // reserved in begin() when the count is known
  n_ = 0;
  return true;
}

bool ExtSort::add(const SortRec &rec) {
  if (!ok_) {
    return false;
  }
  if (n_ == cap_ && !(ok_ = spill())) {
    return false;
  }
  buf_[n_++] = rec;
  total_++;
  return true;
}

bool ExtSort::finish() {
  if (!ok_) {
    return false;
  }
  if (!spilled_) {
    std::sort(buf_, buf_ + n_, recLess);  // everything fits: no temp file
    pos_ = 0;
    return true;
  }
  if (n_ > 0 && !(ok_ = spill())) {
    return false;
  }
  if (f_sync(fil_) != FR_OK) {
    error_ = "temp file sync failed";
    return ok_ = false;
  }
  seek_.build(fil_);  // the file is complete; refills only read now
  // Split the buffer into one slice per run and prime each slice.
  sliceLen_ = std::max<uint32_t>(1, std::min<uint32_t>(cap_ / runs_.size(), 0xFFFF));
  if (sliceLen_ * runs_.size() > cap_) {
    error_ = "more runs than buffer slots";
    return ok_ = false;  // (tens of millions of records)
  }
  heap_.clear();
  for (uint32_t r = 0; r < runs_.size(); r++) {
    runs_[r].slice = r * sliceLen_;
    if (!refill(r)) {
      return ok_ = false;
    }
    if (runs_[r].bufLen > 0) {
      heap_.push_back(r);
    }
  }
  for (size_t i = heap_.size(); i-- > 0;) {
    siftDown(i);
  }
  return true;
}

bool ExtSort::refill(uint32_t r) {
  Run &run = runs_[r];
  const uint32_t want = std::min(sliceLen_, run.count - run.loaded);
  run.bufPos = 0;
  run.bufLen = want;
  if (want == 0) {
    return true;
  }
  UINT got = 0;
  const UINT bytes = want * sizeof(SortRec);
  if (seek_.seek(fil_, static_cast<FSIZE_t>(run.start + run.loaded) * sizeof(SortRec)) != FR_OK ||
      f_read(fil_, buf_ + run.slice, bytes, &got) != FR_OK || got != bytes) {
    return false;
  }
  run.loaded += want;
  return true;
}

bool ExtSort::less(uint32_t a, uint32_t b) const {
  const Run &ra = runs_[a];
  const Run &rb = runs_[b];
  return recLess(buf_[ra.slice + ra.bufPos], buf_[rb.slice + rb.bufPos]);
}

void ExtSort::siftDown(size_t i) {
  const size_t n = heap_.size();
  while (true) {
    size_t smallest = i;
    const size_t l = 2 * i + 1;
    const size_t r = l + 1;
    if (l < n && less(heap_[l], heap_[smallest])) smallest = l;
    if (r < n && less(heap_[r], heap_[smallest])) smallest = r;
    if (smallest == i) {
      return;
    }
    std::swap(heap_[i], heap_[smallest]);
    i = smallest;
  }
}

bool ExtSort::next(SortRec &out) {
  if (!ok_) {
    return false;
  }
  cardYieldPoint();
  if (!spilled_) {
    if (pos_ >= n_) {
      return false;
    }
    out = buf_[pos_++];
    return true;
  }
  if (heap_.empty()) {
    return false;
  }
  const uint32_t r = heap_[0];
  Run &run = runs_[r];
  out = buf_[run.slice + run.bufPos];
  run.bufPos++;
  if (run.bufPos == run.bufLen) {
    if (!refill(r)) {
      return ok_ = false;
    }
    if (run.bufLen == 0) {  // run exhausted
      heap_[0] = heap_.back();
      heap_.pop_back();
    }
  }
  if (!heap_.empty()) {
    siftDown(0);
  }
  return true;
}

void ExtSort::end() {
  if (fil_) {
    if (spilled_) {
      f_close(fil_);
      f_unlink(cardPath(path_).c_str());
    }
    cardDeleteFil(fil_);
    fil_ = nullptr;
  }
  seek_.clear();
  free(buf_);
  buf_ = nullptr;
  runs_.clear();
  heap_.clear();
  cap_ = n_ = total_ = pos_ = sliceLen_ = 0;
  spilled_ = ok_ = false;
}
