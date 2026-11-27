#pragma once
// Reading, writing and generation management for the on-card library
// (format in LibFormat.h). All I/O goes through FatFs directly.

#include "ff.h"

#include "CardFile.h"
#include "LibFormat.h"
#include "Tags.h"

// Copies text into a fixed NUL-terminated record field, truncating to fit.
void libCopyField(char *dst, size_t size, const String &text);  // cut on a UTF-8 character boundary
// Copies up to n bytes of the accent-folded, lower-case form (for sort keys).
void libSortCopy(uint8_t *dst, size_t n, const char *src);

// Fills the tag-derived fields of a song record. `fileName` is the exact
// on-card name (used for the title when the file has no title tag).
void libFillSong(SongRecord &rec, const TagInfo &tags, const char *fileName, const char *folderPath = "");

// Writes one library file: header placeholder, records, extra, then the real
// header. The file is only valid once finish() succeeds.
class LibWriter {
public:
  ~LibWriter() { abort(); cardDeleteFil(fil_); free(wbuf_); }
  bool create(uint32_t gen, LibFileType type, uint16_t recordSize, uint16_t extraSize = 0);
  bool append(const void *records, uint32_t n = 1);
  bool finish(const void *extra = nullptr);  // extra must be extraSize bytes
  void abort();                              // closes without a valid header
  uint32_t count() const { return header_.recordCount; }
  uint32_t crc() const { return crc_; }      // running data CRC so far
  int lastError() const { return fr_; }      // FatFs result of the last failed call (0 = none)

  // Checkpointing: sync() flushes everything appended so far to the card.
  // resume() reopens an unfinished file and continues after `count` records
  // whose running CRC was `crc` (anything after them is truncated).
  bool sync();
  bool resume(uint32_t gen, LibFileType type, uint16_t recordSize, uint32_t count,
              uint32_t crc);

private:
  // Appends go straight to FatFs, whose sector buffer batches small records
  // into single-sector writes (see cardWrite). A RAM buffer here would only
  // cost memory, which the sort phase is short of while the player runs.
  static constexpr uint32_t WRITE_BUFFER = 0;
  bool flush();
  FIL *fil_ = nullptr;  // heap: cardNewFil()
  uint8_t *wbuf_ = nullptr;  // WRITE_BUFFER bytes, or null (unbuffered) if no memory
  uint32_t wlen_ = 0;
  int fr_ = 0;
  bool open_ = false;
  bool ok_ = false;
  LibHeader header_ = {};
  uint32_t crc_ = 0;
};

// Random access to one library file. open() checks the header (magic, type,
// version, record size, header CRC, file length); verify() checks the data CRC.
class LibReader {
public:
  ~LibReader() { close(); cardDeleteFil(fil_); free(rbuf_); }
  bool open(uint32_t gen, LibFileType type, uint16_t recordSize);
  void close();
  bool isOpen() const { return open_; }
  const LibHeader &header() const { return header_; }
  uint32_t count() const { return header_.recordCount; }
  bool read(uint32_t index, void *out, uint32_t n = 1);
  bool readBytes(uint32_t offset, void *out, uint32_t len);  // names.dat (recordSize 1)
  bool readExtra(void *out);
  bool verify();

  // For mostly-sequential scans: a miss reads `bytes` ahead in one go (one
  // multi-block SD read) and later reads are served from RAM. Leave off for
  // random access (it would read data that is never used). Library files
  // never change once written, so the buffer can't go stale.
  void setReadAhead(uint32_t bytes);
  // With read-ahead: makes records [index, index + n) one buffered range
  // (filling from `index` if they aren't all there and fit), e.g. a folder's
  // songs before reading them twice. Otherwise a no-op.
  void prefetch(uint32_t index, uint32_t n);
  // Reads the whole file into PSRAM and closes it on the card: from then on
  // every read is a copy from RAM. False (and nothing changes) if there's no
  // PSRAM for it.
  bool loadToRam();
  bool inRam() const { return whole_; }
  uint32_t ramBytes() const { return whole_ ? rcap_ : 0; }

private:
  FIL *fil_ = nullptr;  // heap: cardNewFil()
  uint8_t *rbuf_ = nullptr;
  uint32_t rcap_ = 0, rpos_ = 0, rlen_ = 0;  // buffer holds file bytes [rpos_, rpos_ + rlen_)
  SeekIndex seek_;
  bool open_ = false;
  bool whole_ = false;  // loadToRam(): rbuf_ holds the whole file, fil_ is closed
  LibHeader header_ = {};
};

namespace LibStore {

String genPath(uint32_t gen);                     // Arduino-style path of a generation folder
String filePath(uint32_t gen, LibFileType type);  // Arduino-style path of a file in it

// Newest valid generation, or LIB_NONE. Header-level checks only (fast).
uint32_t findCurrent(ManifestRecord *manifest = nullptr);
bool validateGeneration(uint32_t gen, ManifestRecord *manifest = nullptr);

// Full data-CRC check of every file in a generation (reads everything).
bool verifyGeneration(uint32_t gen);

// Creates an empty folder for the next generation. LIB_NONE on failure.
uint32_t newGeneration();

// Reads back every file's data CRC, then writes the manifest, which makes the
// generation live. `manifest` supplies the counts; CRCs are filled in here.
bool commit(uint32_t gen, ManifestRecord manifest);

// Highest generation number on the card, valid or not (LIB_NONE if none).
uint32_t newestGeneration();

bool removeGeneration(uint32_t gen);
void removeAllExcept(uint32_t keep, uint32_t keepToo = LIB_NONE);

}  // namespace LibStore
