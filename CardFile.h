#pragma once
// Direct FatFs file access, bypassing Arduino's fs::File / stdio layer.
//
// Arduino's File goes through VFS + stdio, which refills a 4 KB buffer after
// every seek; tag parsing seeks a lot. FatFs only reads the 512-byte sectors
// it needs. The SD library mounts the card as FatFs drive 0 ("0:"), verified
// by Bench.

#include <FS.h>
#include <vector>
#include "ff.h"

#include "Tags.h"

#define CARD_DRIVE "0:"

// FatFs in this core is built for 4 KB sectors (flash wear levelling), so a
// FIL carries a 4 KB buffer, but an SD card's sectors are 512 bytes and FatFs
// never touches more than one sector of it (f_open clears fs->ssize bytes;
// reads and writes use the volume's sector size). cardNewFil() allocates a
// FIL with a 512-byte buffer once cardNoteMount() has seen 512-byte sectors
// (3.5 KB less per open file), else a full one. Zeroed; free with
// cardDeleteFil(). Only for files on the card.
void cardNoteMount();  // call after each successful mount
FIL *cardNewFil();
void cardDeleteFil(FIL *fil);

// Converts an Arduino SD path ("/a/b.mp3") to a FatFs path ("0:/a/b.mp3").
String cardPath(const String &path);

// Fast open: call right after f_readdir() returned a file. Reads the entry's
// start cluster from the directory sector FatFs still holds in its window,
// after checking the raw entry matches `info` (attributes, size, date, time).
// Returns false when it can't be confirmed; then open by path instead.
// Avoids f_open's name lookup, which scans the folder from the start
// (7-11 ms per file measured, growing with folder size).
bool cardCaptureEntry(const FF_DIR &dir, const FILINFO &info, uint32_t &startCluster);

// Background indexing runs on core 0 above the idle task (which feeds the
// task watchdog). Card I/O helpers call cardYieldPoint(); for that task it
// sleeps one tick every 50 ms. No effect on other tasks.
void cardSetYieldTask(void *taskHandle);
void cardYieldPoint();

// f_write, but never more than one sector per call, so FatFs writes through
// its sector buffer and the SD driver only uses single-block writes. Its
// multi-block path (CMD25) failed now and then in the player, while audio
// and the LCD shared the bus, with no driver message (FR_DISK_ERR); single
// blocks were reliable there.
FRESULT cardWrite(FIL *fil, const void *data, UINT len, UINT *written);

// Fast random seeks in a file that no longer grows. FatFs follows the
// cluster chain from the start of the file for every backward seek (its own
// fast-seek option isn't compiled into this core): thousands of links in a
// 12 MB library file, through a one-sector FAT cache other files also use.
// build() walks the chain once and keeps every STRIDE-th cluster; seek()
// starts from the nearest one (at most STRIDE links). It sets the same two
// FIL fields f_lseek maintains (fptr, clust) to an earlier position on the
// same chain, then lets f_lseek finish the job.
class SeekIndex {
public:
  void build(FIL *fil);  // file position ends at 0
  void clear() { std::vector<uint32_t>().swap(anchors_); }
  FRESULT seek(FIL *fil, FSIZE_t pos);

private:
  static constexpr uint32_t STRIDE = 16;
  std::vector<uint32_t> anchors_;  // [i]: cluster number of cluster index i*STRIDE-1
  uint32_t clusterBytes_ = 0;
};

// Same for a subfolder returned by f_readdir (for CardDir::open).
bool cardCaptureDir(const FF_DIR &dir, const FILINFO &info, uint32_t &startCluster);

// Lists a folder. Opening by path makes FatFs search the parent folder for
// the name (about 100 ms per folder in a folder of 1600 artists); opening by
// a cluster captured while the parent was listed skips that. The first read
// confirms the folder's "." entry points back at that cluster; if it can't,
// the folder is reopened by path and listed from the start.
class CardDir {
public:
  ~CardDir() { close(); }
  // fatfsPath like "0:/Music/Artist"; cluster 0 opens by path.
  bool open(const char *fatfsPath, uint32_t cluster);
  // Next entry (FatFs skips "." and ".."); false at the end or on error.
  bool read(FILINFO &info);
  void close();
  const FF_DIR &dir() const { return dir_; }

  // Folders opened by cluster / by path since boot (by path includes fallbacks).
  static uint32_t clusterOpens, pathOpens;

private:
  bool reopenByPath();
  FF_DIR dir_;
  const char *path_ = nullptr;  // caller keeps it alive while open
  uint32_t cluster_ = 0;
  bool open_ = false;
  bool verify_ = false;
};

class CardFile : public TagSource {
public:
  ~CardFile() override { close(); cardDeleteFil(fil_); free(cache_); }
  // Optional block cache for tag parsing: a miss reads `bytes` from the
  // sector holding the request in one go; small reads nearby then come from
  // RAM. 0 turns it off. Kept across open() calls.
  void setCache(uint32_t bytes);
  // Since boot: readAt() calls and the card reads they caused.
  static uint32_t readCalls, cardReads;
  bool open(const String &path);  // Arduino-style path
  // Opens a file captured by cardCaptureEntry() during this mount, read-only.
  bool openCluster(uint32_t startCluster, uint32_t size);
  void close();
  uint32_t size() override;
  size_t readAt(uint32_t pos, void *buf, size_t len) override;

private:
  FIL *fil_ = nullptr;  // heap: a FIL carries a 4 KB sector buffer
  bool open_ = false;
  uint8_t *cache_ = nullptr;
  uint32_t ccap_ = 0, cpos_ = 0, clen_ = 0;  // cache holds file bytes [cpos_, cpos_ + clen_)
};

// TagSource over an Arduino fs::File (used by Bench for comparison).
class FsFileSource : public TagSource {
public:
  explicit FsFileSource(fs::File &f) : f_(f) {}
  uint32_t size() override { return f_.size(); }
  size_t readAt(uint32_t pos, void *buf, size_t len) override {
    if (!f_.seek(pos)) {
      return 0;
    }
    return f_.read(static_cast<uint8_t *>(buf), len);
  }

private:
  fs::File &f_;
};
