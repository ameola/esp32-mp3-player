#include "Indexer.h"
#include <new>

#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <algorithm>
#include <vector>
#include "ff.h"

#include "CardFile.h"
#include "ExtSort.h"
#include "LibClean.h"
#include "LibStore.h"
#include "Library.h"
#include "Tags.h"
#include "Text.h"

#define SORT_LIMIT 256         // folders with more entries are listed in directory order (always, so
                               // builds agree: the sort-key patch needs the same folder order)
#define CHECKPOINT_EVERY 256
#define SORT_BUFFER_MAX (64 * 1024)   // per sorter (title, artist), if memory allows
#define SORT_BUFFER_MIN (16 * 1024)
#define ALBUM_SORT_BUFFER (16 * 1024)
#define HEAP_RESERVE (24 * 1024)      // largest free block left for the player while sorting
// Total heap that must stay free after both sort buffers: the sort phase then
// still opens up to 5 files (FIL + write buffer each), the album sorter and
// its run file, while the player keeps decoding.
#define SORT_FREE_AFTER (64 * 1024)
// Margin for growing small working arrays (patch ranges, a folder's entries):
// enough that the growth itself can't run the heap dry.
#define SMALL_RESERVE (4 * 1024)
#define MAX_DEPTH 8
#define MAX_PATH_BYTES 1024

static const char kVarious[] = "Various Artists";
static const char kUnknownArtist[] = "Unknown Artist";

// One entry per song from the walk (temp file).
struct __attribute__((packed)) SkelRecord {
  uint32_t folderId;
  uint32_t nameOffset;
  uint16_t nameLen;
  uint16_t reserved;
  uint32_t size;
  uint32_t mtime;
  uint32_t cluster;  // start cluster for a fast open, 0 if not captured
};
static_assert(sizeof(SkelRecord) == 24, "SkelRecord size");

// Phase B resume point (temp file).
struct __attribute__((packed)) Checkpoint {
  char magic[4];
  uint32_t listingSum;
  uint32_t listingSum2;
  uint32_t songCount;
  uint32_t done;
  uint32_t songsCrc;
  uint32_t oldGen;
  uint32_t crc;
};

// Order-independent fingerprint of the MP3 listing, so the read-only check
// (depth-first, unsorted) and the walk (breadth-first, sorted) agree.
struct Fingerprint {
  uint32_t sum = 0;
  uint32_t sum2 = 0;
  uint32_t count = 0;
  void add(const char *folder, size_t folderLen, const char *name, size_t nameLen,
           uint32_t size, uint32_t mtime);
  bool operator==(const Fingerprint &o) const {
    return sum == o.sum && sum2 == o.sum2 && count == o.count;
  }
};

struct WalkResult {
  Fingerprint fp;
  uint32_t songs = 0;
  uint32_t folders = 0;
  uint32_t nameBytes = 0;
};

// ================================================================ Helpers
static uint32_t crc32(uint32_t crc, const void *data, size_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t *>(data), len);
}

static uint32_t fnv1a(const char *s, size_t n, bool lower) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n && s[i]; i++) {
    const uint8_t c = lower ? tolower(static_cast<unsigned char>(s[i])) : s[i];
    h = (h ^ c) * 16777619u;
  }
  return h;
}

static uint32_t hashLower(const char *s) {
  return fnv1a(s, SIZE_MAX, true);
}

void Fingerprint::add(const char *folder, size_t folderLen, const char *name, size_t nameLen,
                      uint32_t size, uint32_t mtime) {
  uint32_t c = crc32(0, folder, folderLen);
  c = crc32(c, "/", 1);
  c = crc32(c, name, nameLen);
  c = crc32(c, &size, sizeof(size));
  c = crc32(c, &mtime, sizeof(mtime));
  uint32_t h = fnv1a(folder, folderLen, false);
  h = (h ^ '/') * 16777619u;
  for (size_t i = 0; i < nameLen; i++) h = (h ^ static_cast<uint8_t>(name[i])) * 16777619u;
  h = (h ^ size) * 16777619u;
  h = (h ^ mtime) * 16777619u;
  sum += c;
  sum2 += h;
  count++;
}

static void putBe16(uint8_t *p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}

static void putBe32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static uint32_t getBe32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void lowerCopy(uint8_t *dst, size_t n, const char *src) {
  libSortCopy(dst, n, src);  // accents folded: "Édith" sorts with "Edith"
}

static const char *stripThe(const char *s) {
  return (strncasecmp(s, "the ", 4) == 0 && s[4]) ? s + 4 : s;
}

static String tempPath(uint32_t gen, const char *name) {
  return LibStore::genPath(gen) + "/" + name;
}

static void report(const IndexOptions &o, const char *phase, uint32_t done, uint32_t total,
                   const char *detail = nullptr) {
  if (o.progress) {
    o.progress(phase, done, total, detail);
  }
}

static String readName(LibReader &names, uint32_t offset, uint16_t len) {
  static char buf[MAX_PATH_BYTES + 1];
  len = std::min<uint16_t>(len, MAX_PATH_BYTES);
  if (len == 0 || !names.readBytes(sizeof(LibHeader) + offset, buf, len)) {
    return "";
  }
  buf[len] = 0;
  return String(buf);
}

// Fills start[] as positions arrive in bucket order. Bucket 0xFF ("unknown")
// lands after Z.
struct JumpBuilder {
  JumpTable table = {};
  uint32_t next = 0;
  void add(uint8_t bucket, uint32_t pos) {
    const uint32_t b = bucket > 26 ? 27 : bucket;
    while (next <= b && next <= 26) {
      table.start[next++] = pos;
    }
  }
  void finish(uint32_t total) {
    while (next <= 26) {
      table.start[next++] = total;
    }
    table.start[27] = total;
  }
};

// ================================================================ Phase A: walk
// FIFO of folders still to list, kept on the card (a wide library can have
// thousands pending at once).
// FIFO of folders still to list (breadth-first walk), on the card so its size
// doesn't depend on the library. Pushes are collected in RAM and written in
// blocks; pops read through their own buffer (or straight from the unwritten
// pushes), so the file isn't flushed and re-read for every folder.
class QueueFile {
public:
  ~QueueFile() { close(); }
  bool open(const String &path) {
    path_ = path;
    fil_ = cardNewFil();
    wbuf_ = static_cast<uint8_t *>(malloc(BUF));
    rbuf_ = static_cast<uint8_t *>(malloc(BUF));
    flushed_ = wlen_ = rpos_ = rlen_ = readPos_ = 0;
    return fil_ && wbuf_ && rbuf_ &&
           f_open(fil_, cardPath(path).c_str(), FA_CREATE_ALWAYS | FA_READ | FA_WRITE) == FR_OK;
  }
  // cluster: the folder's start cluster if captured (CardDir), else 0.
  bool push(uint32_t id, uint32_t parent, uint16_t depth, uint32_t cluster, const String &path) {
    uint8_t h[16];
    memcpy(h, &id, 4);
    memcpy(h + 4, &parent, 4);
    memcpy(h + 8, &depth, 2);
    const uint16_t len = path.length();
    memcpy(h + 10, &len, 2);
    memcpy(h + 12, &cluster, 4);
    return put(h, 16) && put(path.c_str(), len);
  }
  bool pop(uint32_t &id, uint32_t &parent, uint16_t &depth, uint32_t &cluster, String &path) {
    if (readPos_ >= flushed_ + wlen_) {
      return false;
    }
    uint8_t h[16];
    static char buf[MAX_PATH_BYTES + 1];
    uint16_t len;
    if (!get(h, 16)) {
      return false;
    }
    memcpy(&id, h, 4);
    memcpy(&parent, h + 4, 4);
    memcpy(&depth, h + 8, 2);
    memcpy(&len, h + 10, 2);
    memcpy(&cluster, h + 12, 4);
    if (len > MAX_PATH_BYTES || !get(buf, len)) {
      return false;
    }
    buf[len] = 0;
    path = buf;
    return true;
  }
  void close() {
    if (fil_) {
      f_close(fil_);
      cardDeleteFil(fil_);
      fil_ = nullptr;
      f_unlink(cardPath(path_).c_str());
    }
    free(wbuf_);
    free(rbuf_);
    wbuf_ = rbuf_ = nullptr;
  }

private:
  static constexpr uint32_t BUF = 2048;  // > 16 + MAX_PATH_BYTES
  bool flush() {
    UINT n = 0;
    if (wlen_ == 0) {
      return true;
    }
    FRESULT fr = f_lseek(fil_, flushed_);
    if (fr == FR_OK) {
      fr = cardWrite(fil_, wbuf_, wlen_, &n);
    }
    if (fr != FR_OK || n != wlen_) {
      fr_ = fr != FR_OK ? fr : 100;
      return false;
    }
    flushed_ += wlen_;
    wlen_ = 0;
    return true;
  }
  bool put(const void *data, uint32_t n) {
    if (wlen_ + n > BUF && !flush()) {
      return false;
    }
    memcpy(wbuf_ + wlen_, data, n);
    wlen_ += n;
    return true;
  }
  // Reads the next n bytes: from the file (via rbuf_) or the unwritten pushes.
  bool get(void *out, uint32_t n) {
    uint8_t *dst = static_cast<uint8_t *>(out);
    while (n > 0) {
      uint32_t chunk;
      if (readPos_ >= flushed_) {
        const uint32_t at = readPos_ - flushed_;
        if (at >= wlen_) {
          return false;
        }
        chunk = std::min(n, wlen_ - at);
        memcpy(dst, wbuf_ + at, chunk);
      } else {
        if (readPos_ < rpos_ || readPos_ >= rpos_ + rlen_) {
          UINT got = 0;
          const UINT want = std::min<uint32_t>(BUF, flushed_ - readPos_);
          if (f_lseek(fil_, readPos_) != FR_OK || f_read(fil_, rbuf_, want, &got) != FR_OK ||
              got != want) {
            return false;
          }
          rpos_ = readPos_;
          rlen_ = got;
        }
        chunk = std::min(n, rpos_ + rlen_ - readPos_);
        memcpy(dst, rbuf_ + (readPos_ - rpos_), chunk);
      }
      dst += chunk;
      readPos_ += chunk;
      n -= chunk;
    }
    return true;
  }
public:
  int lastError() const { return fr_; }

private:
  int fr_ = 0;
  FIL *fil_ = nullptr;
  String path_;
  uint8_t *wbuf_ = nullptr, *rbuf_ = nullptr;
  uint32_t flushed_ = 0, wlen_ = 0;  // file holds [0, flushed_); wbuf_ the next wlen_ bytes
  uint32_t rpos_ = 0, rlen_ = 0;     // rbuf_ holds file bytes [rpos_, rpos_ + rlen_)
  uint32_t readPos_ = 0;
};

struct WalkEntry {
  String name;
  uint32_t size;
  uint32_t mtime;
  uint32_t cluster;
  bool dir;
};

// Breadth-first: when a folder is listed its subfolders get consecutive ids
// and its songs consecutive skeleton positions, so both are contiguous.
static bool walk(uint32_t gen, const IndexOptions &o, WalkResult &result, IndexStats &st) {
  LibWriter names, folders, skel;
  QueueFile queue;
  if (!names.create(gen, LF_NAMES, 1) || !folders.create(gen, LF_FOLDERS, sizeof(FolderRecord)) ||
      !skel.create(gen, LF_TEMP_SKELETON, sizeof(SkelRecord)) ||
      !queue.open(tempPath(gen, "queue.tmp")) || !queue.push(0, LIB_NONE, 0, 0, "")) {
    st.error = "walk: cannot create files";
    return false;
  }
  uint32_t nextId = 1;
  uint32_t nameBytes = 0;
  Fingerprint fp;
  std::vector<WalkEntry> entries;
  static FILINFO info;

  uint32_t id, parent, dirCluster;
  uint16_t depth;
  String path;
  while (queue.pop(id, parent, depth, dirCluster, path)) {
    FolderRecord frec = {};
    frec.parentId = parent;
    frec.depth = depth;
    frec.firstChild = nextId;
    frec.firstSong = skel.count();
    bool ok = true;

    auto emit = [&](const WalkEntry &e) {
      if (e.dir) {
        frec.childCount++;
        ok = ok && queue.push(nextId++, id, depth + 1, e.cluster, path + "/" + e.name);
        return;
      }
      SkelRecord rec = {id, nameBytes, static_cast<uint16_t>(e.name.length()), 0,
                        e.size, e.mtime, e.cluster};
      fp.add(path.c_str(), path.length(), e.name.c_str(), e.name.length(), e.size, e.mtime);
      ok = ok && names.append(e.name.c_str(), e.name.length()) && skel.append(&rec);
      nameBytes += e.name.length();
      frec.songCount++;
    };

    entries.clear();
    bool streaming = false;  // huge folder: emit in directory order instead of sorting
    CardDir dir;
    const String fatfsPath = cardPath(path.isEmpty() ? "/" : path);
    if (dir.open(fatfsPath.c_str(), dirCluster)) {
      while (ok && dir.read(info)) {
        if (info.fattrib & (AM_HID | AM_SYS)) {
          continue;
        }
        WalkEntry e;
        if (info.fattrib & AM_DIR) {
          const bool isBench = strcmp(info.fname, "_bench") == 0;
          if (depth >= MAX_DEPTH || (isBench ? !o.includeBench : librarySkipFolder(info.fname))) {
            continue;
          }
          uint32_t cluster = 0;
          cardCaptureDir(dir.dir(), info, cluster);
          e = {info.fname, 0, 0, cluster, true};
        } else if (libraryIsMp3Name(info.fname)) {
          uint32_t cluster = 0;
          cardCaptureEntry(dir.dir(), info, cluster);
          e = {info.fname, static_cast<uint32_t>(info.fsize),
               (static_cast<uint32_t>(info.fdate) << 16) | info.ftime, cluster, false};
        } else {
          continue;
        }
        if (!streaming) {
          // Growing the vector needs the old and the new array at once; while
          // the player runs the heap may not have that. Then (or past
          // SORT_LIMIT) this folder is listed in directory order instead.
          const size_t cap = entries.capacity();
          const bool full =
            entries.size() >= SORT_LIMIT ||
            (entries.size() == cap && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) <
                                        (cap * 2 + 16) * sizeof(WalkEntry) + SMALL_RESERVE);
          if (full) {
            streaming = true;
            for (const WalkEntry &x : entries) {
              emit(x);
            }
            std::vector<WalkEntry>().swap(entries);  // give the memory back
          }
        }
        if (streaming) {
          emit(e);
        } else {
          entries.push_back(std::move(e));
        }
      }
      dir.close();
    }
    if (!streaming) {
      std::sort(entries.begin(), entries.end(), [](const WalkEntry &a, const WalkEntry &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
      });
      for (const WalkEntry &e : entries) {
        emit(e);
      }
    }

    frec.pathOffset = nameBytes;
    frec.pathLen = path.length();
    const String leaf = path.isEmpty() ? String("/") : path.substring(path.lastIndexOf('/') + 1);
    libCopyField(frec.name, sizeof(frec.name),
                 decodeUtf8(reinterpret_cast<const uint8_t *>(leaf.c_str()), leaf.length()));
    ok = ok && names.append(path.c_str(), path.length()) && folders.append(&frec);
    nameBytes += path.length();
    if (!ok) {
      st.error = String("walk: write failed (FatFs: names ") + names.lastError() + ", folders " +
                 folders.lastError() + ", skeleton " + skel.lastError() + ", queue " +
                 queue.lastError() + "; free heap " + ESP.getFreeHeap() + ")";
      return false;
    }
    report(o, "Scanning folders", skel.count(), 0, path.isEmpty() ? "/" : path.c_str());
  }
  queue.close();
  result.fp = fp;
  result.songs = skel.count();
  result.folders = folders.count();
  result.nameBytes = nameBytes;
  if (!names.finish() || !folders.finish() || !skel.finish()) {
    st.error = "walk: finish failed";
    return false;
  }
  return true;
}

// ================================================================ Phase B: songs
static FIL *checkpointFil = nullptr;

static bool readCheckpoint(uint32_t gen, Checkpoint &cp) {
  FIL *fil = cardNewFil();
  if (!fil) {
    return false;
  }
  UINT n = 0;
  bool ok = f_open(fil, cardPath(tempPath(gen, "progress.tmp")).c_str(), FA_READ) == FR_OK;
  if (ok) {
    ok = f_read(fil, &cp, sizeof(cp), &n) == FR_OK && n == sizeof(cp) &&
         memcmp(cp.magic, "IXCP", 4) == 0 && cp.crc == crc32(0, &cp, offsetof(Checkpoint, crc));
    f_close(fil);
  }
  cardDeleteFil(fil);
  return ok;
}

static bool writeCheckpoint(Checkpoint cp) {
  memcpy(cp.magic, "IXCP", 4);
  cp.crc = crc32(0, &cp, offsetof(Checkpoint, crc));
  UINT n = 0;
  return f_lseek(checkpointFil, 0) == FR_OK &&
         f_write(checkpointFil, &cp, sizeof(cp), &n) == FR_OK && n == sizeof(cp) &&
         f_sync(checkpointFil) == FR_OK;
}

struct OldEntry {
  uint32_t hash;
  uint32_t size;
  uint32_t mtime;
  uint32_t id;
};

// New folder id -> its folder id in the live library (LIB_NONE if new). Kept
// on the card: for a big library one heap block of 4 bytes per folder may not
// exist while the player runs (the heap is fragmented). Ids are added in
// order; lookups (parents) only go forward, so the FIL buffer serves most.
class FolderMap {
public:
  ~FolderMap() { close(); }
  bool open(const String &path) {
    path_ = path;
    fil_ = cardNewFil();
    if (!fil_) {
      error = "no memory (largest block " + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + ")";
      return false;
    }
    const FRESULT r = f_open(fil_, cardPath(path).c_str(), FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
    if (r != FR_OK) {
      error = "FatFs error " + String(r);
      cardDeleteFil(fil_);
      fil_ = nullptr;
      return false;
    }
    return true;
  }
  String error;
  bool add(uint32_t oldId) {
    buf_[pending_++] = oldId;
    return pending_ < BUF || flush();
  }
  uint32_t get(uint32_t id) {
    if (id >= flushed_) {
      return id - flushed_ < pending_ ? buf_[id - flushed_] : LIB_NONE;
    }
    uint32_t v = LIB_NONE;
    UINT n = 0;
    if (f_lseek(fil_, id * 4) != FR_OK || f_read(fil_, &v, 4, &n) != FR_OK || n != 4) {
      return LIB_NONE;
    }
    return v;
  }
  void close() {
    if (fil_) {
      f_close(fil_);
      cardDeleteFil(fil_);
      fil_ = nullptr;
      f_unlink(cardPath(path_).c_str());
    }
  }

private:
  static constexpr uint32_t BUF = 256;
  bool flush() {
    UINT n = 0;
    const bool ok = f_lseek(fil_, flushed_ * 4) == FR_OK &&
                    cardWrite(fil_, buf_, pending_ * 4, &n) == FR_OK && n == pending_ * 4;
    flushed_ += pending_;
    pending_ = 0;
    return ok;
  }
  FIL *fil_ = nullptr;
  String path_;
  uint32_t buf_[BUF];
  uint32_t flushed_ = 0, pending_ = 0;
};

// Finds `path` among an old parent's children [begin, end), starting at
// `cursor` and wrapping. Both walks list a folder's children in the same
// order, so an unchanged folder is found at the cursor with one read.
static uint32_t findOldFolder(LibReader &oldFolders, LibReader &oldNames, const String &path,
                              uint32_t begin, uint32_t end, uint32_t &cursor, FolderRecord &out) {
  const uint32_t n = end - begin;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t c = cursor + i;
    if (c >= end) {
      c -= n;
    }
    if (oldFolders.read(c, &out) && out.pathLen == path.length() &&
        readName(oldNames, out.pathOffset, out.pathLen) == path) {
      cursor = c + 1 < end ? c + 1 : begin;
      return c;
    }
  }
  return LIB_NONE;
}

// True if the old song record `rec` is the file `name` with this size/time.
static bool sameSong(LibReader &oldNames, const SongRecord &rec, const String &name,
                     const SkelRecord &srec) {
  return rec.fileSize == srec.size && rec.mtime == srec.mtime && rec.nameLen == name.length() &&
         readName(oldNames, rec.nameOffset, rec.nameLen) == name;
}

// Which songs carried over unchanged from the live library, for patching its
// sorted keys instead of re-sorting (sortAndGroup). A folder counts as
// unchanged when it has the same songs, in the same order, all reused; its
// songs then keep their sort keys and only their ids shift. Unchanged folders
// are kept as ranges of song ids, merged when adjacent.
struct KeptRange {
  uint32_t oldFirst, newFirst, count;
};

struct PatchInfo {
  static constexpr size_t MAX_RANGES = 2048;  // 24 KB; beyond that just re-sort
  bool usable = false;
  const char *why = "";  // why it was dropped
  uint32_t keptSongs = 0;
  std::vector<KeptRange> kept;  // increasing in both old and new ids

  void keep(uint32_t oldFirst, uint32_t newFirst, uint32_t count) {
    if (!usable) {
      return;
    }
    if (!kept.empty()) {
      KeptRange &last = kept.back();
      if (last.oldFirst + last.count == oldFirst && last.newFirst + last.count == newFirst) {
        last.count += count;
        keptSongs += count;
        return;
      }
      if (oldFirst < last.oldFirst + last.count) {  // order changed: ids not monotonic
        drop("folders changed order");
        return;
      }
    }
    // (Growing needs old + new array at once; if the heap can't, re-sort.)
    if (kept.size() >= MAX_RANGES ||
        (kept.size() == kept.capacity() &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) <
           (kept.capacity() * 2 + 16) * sizeof(KeptRange) + SMALL_RESERVE)) {
      drop(kept.size() >= MAX_RANGES ? "too many changed folders" : "no memory for ranges");
      return;
    }
    kept.push_back({oldFirst, newFirst, count});
    keptSongs += count;
  }
  void drop(const char *reason = "") {
    if (usable && reason[0]) {
      why = reason;
    }
    usable = false;
    keptSongs = 0;
    std::vector<KeptRange>().swap(kept);
  }
  // New id of a kept old song, or LIB_NONE if its folder changed.
  uint32_t newId(uint32_t oldId) const {
    auto it = std::upper_bound(kept.begin(), kept.end(), oldId,
                               [](uint32_t v, const KeptRange &r) { return v < r.oldFirst; });
    if (it == kept.begin()) {
      return LIB_NONE;
    }
    --it;
    return oldId - it->oldFirst < it->count ? it->newFirst + (oldId - it->oldFirst) : LIB_NONE;
  }
  // True if new song id `id` belongs to an unchanged folder.
  bool isKeptNew(uint32_t id) const {
    auto it = std::upper_bound(kept.begin(), kept.end(), id,
                               [](uint32_t v, const KeptRange &r) { return v < r.newFirst; });
    return it != kept.begin() && id - (it - 1)->newFirst < (it - 1)->count;
  }
};

static bool buildSongs(uint32_t gen, uint32_t oldGen, const WalkResult &w, const IndexOptions &o,
                       IndexStats &st, PatchInfo &patch) {
  LibReader newFolders, newNames, skel;
  if (!newFolders.open(gen, LF_FOLDERS, sizeof(FolderRecord)) || !newNames.open(gen, LF_NAMES, 1) ||
      !skel.open(gen, LF_TEMP_SKELETON, sizeof(SkelRecord))) {
    st.error = "songs: cannot open walk output";
    return false;
  }
  // All of these are read (nearly) in order.
  newFolders.setReadAhead(2048);
  newNames.setReadAhead(2048);
  skel.setReadAhead(2048);
  LibReader oldFolders, oldNames, oldSongs;
  const bool haveOld = oldGen != LIB_NONE &&
                       oldFolders.open(oldGen, LF_FOLDERS, sizeof(FolderRecord)) &&
                       oldNames.open(oldGen, LF_NAMES, 1) &&
                       oldSongs.open(oldGen, LF_SONGS, sizeof(SongRecord));
  oldFolders.setReadAhead(2048);
  oldNames.setReadAhead(2048);
  oldSongs.setReadAhead(8192);

  // Resume from a checkpoint made by an interrupted run over the same listing.
  LibWriter songs;
  uint32_t start = 0;
  Checkpoint cp;
  if (readCheckpoint(gen, cp) && cp.listingSum == w.fp.sum && cp.listingSum2 == w.fp.sum2 &&
      cp.songCount == w.songs &&
      cp.oldGen == oldGen && cp.done <= w.songs &&
      songs.resume(gen, LF_SONGS, sizeof(SongRecord), cp.done, cp.songsCrc)) {
    start = cp.done;
    st.resumed = true;
    st.resumedAt = start;
  } else if (!songs.create(gen, LF_SONGS, sizeof(SongRecord))) {
    st.error = "songs: cannot create songs.dat";
    return false;
  }
  if (checkpointFil) {  // left open by an attempt that ended in an exception
    f_close(checkpointFil);
    cardDeleteFil(checkpointFil);
  }
  checkpointFil = cardNewFil();
  if (!checkpointFil || f_open(checkpointFil, cardPath(tempPath(gen, "progress.tmp")).c_str(),
             FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) {
    cardDeleteFil(checkpointFil);
    checkpointFil = nullptr;
    st.error = "songs: cannot create progress.tmp";
    return false;
  }

  // Each new folder's old counterpart, found by stepping through the old
  // tree in step with the new one (see findOldFolder). No per-library tables
  // in RAM: see FolderMap.
  FolderMap folderMap;
  if (!folderMap.open(tempPath(gen, "foldermap.tmp"))) {
    st.error = "songs: cannot create foldermap.tmp: " + folderMap.error;
    return false;
  }
  uint32_t curParent = LIB_NONE, kidsBegin = 0, kidsEnd = 0, kidsCursor = 0;

  std::vector<OldEntry> table;  // big changed folders only, if memory allows
  CardFile card;
  // Tag parsing makes ~29 small reads per song; a 2 KB block cache turns
  // them into ~2 card reads (measured on real files: 7.3 -> 5.2 ms per song;
  // 4 and 8 KB were slower: the extra transfer outweighs the saved commands).
  card.setCache(2048);
  FolderRecord frec, oldRec;
  SkelRecord srec;
  SongRecord rec;
  bool ok = true;
  uint32_t tagUs = 0;
  patch.usable = haveOld && start == 0;
  if (patch.usable) {
    patch.kept.reserve(64);  // most updates need a handful of ranges
  }
  for (uint32_t f = 0; ok && f < newFolders.count(); f++) {
    newFolders.read(f, &frec);
    const String path = readName(newNames, frec.pathOffset, frec.pathLen);

    uint32_t oldId = LIB_NONE;
    if (haveOld) {
      if (f == 0) {
        oldId = oldFolders.count() && oldFolders.read(0, &oldRec) ? 0 : LIB_NONE;
      } else {
        if (frec.parentId != curParent) {
          curParent = frec.parentId;
          const uint32_t oldParent = folderMap.get(curParent);
          FolderRecord pr;
          kidsBegin = kidsEnd = kidsCursor = 0;
          if (oldParent != LIB_NONE && oldFolders.read(oldParent, &pr)) {
            kidsBegin = kidsCursor = pr.firstChild;
            kidsEnd = pr.firstChild + pr.childCount;
          }
        }
        oldId = findOldFolder(oldFolders, oldNames, path, kidsBegin, kidsEnd, kidsCursor, oldRec);
      }
    }
    if (!folderMap.add(oldId)) {
      st.error = "songs: foldermap write failed";
      ok = false;
      break;
    }

    const uint32_t end = frec.firstSong + frec.songCount;
    if (frec.songCount == 0 || end <= start) {
      continue;
    }
    // This folder's songs in the live library, matched with a cursor (one
    // read per unchanged song); a miss scans the old folder, through a hash
    // table for big folders when there is memory for one.
    const uint32_t oldFirst = oldId != LIB_NONE ? oldRec.firstSong : 0;
    const uint32_t oldEnd = oldId != LIB_NONE ? oldRec.firstSong + oldRec.songCount : 0;
    uint32_t songCursor = oldFirst;
    oldSongs.prefetch(oldFirst, oldEnd - oldFirst);
    skel.prefetch(std::max(frec.firstSong, start), end - std::max(frec.firstSong, start));
    table.clear();
    bool tableTried = false;
    bool folderSame = oldId != LIB_NONE && oldRec.songCount == frec.songCount;

    for (uint32_t s = std::max(frec.firstSong, start); ok && s < end; s++) {
      skel.read(s, &srec);
      const String name = readName(newNames, srec.nameOffset, srec.nameLen);
      bool reused = false;
      if (oldFirst < oldEnd) {
        if (songCursor < oldEnd && oldSongs.read(songCursor, &rec) &&
            sameSong(oldNames, rec, name, srec)) {
          reused = true;
          songCursor++;
        } else {
          const uint32_t count = oldEnd - oldFirst;
          if (!tableTried && count > 64) {
            tableTried = true;
            const size_t need = count * sizeof(OldEntry);
            if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > need + 32 * 1024) {
              table.reserve(count);
              for (uint32_t i = oldFirst; i < oldEnd; i++) {
                oldSongs.read(i, &rec);
                const String n = readName(oldNames, rec.nameOffset, rec.nameLen);
                table.push_back({fnv1a(n.c_str(), n.length(), false), rec.fileSize, rec.mtime, i});
              }
              std::sort(table.begin(), table.end(),
                        [](const OldEntry &a, const OldEntry &b) { return a.hash < b.hash; });
            }
          }
          if (!table.empty()) {
            const uint32_t h = fnv1a(name.c_str(), name.length(), false);
            auto it = std::lower_bound(table.begin(), table.end(), h,
                                       [](const OldEntry &e, uint32_t v) { return e.hash < v; });
            for (; !reused && it != table.end() && it->hash == h; ++it) {
              reused = it->size == srec.size && it->mtime == srec.mtime &&
                       oldSongs.read(it->id, &rec) && sameSong(oldNames, rec, name, srec);
              if (reused) {
                songCursor = it->id + 1;
              }
            }
          } else {
            for (uint32_t i = oldFirst; !reused && i < oldEnd; i++) {
              reused = oldSongs.read(i, &rec) && sameSong(oldNames, rec, name, srec);
              if (reused) {
                songCursor = i + 1;
              }
            }
          }
        }
      }
      // Every match leaves songCursor just past the old song it matched.
      folderSame = folderSame && reused && songCursor - 1 == oldFirst + (s - frec.firstSong);
      if (reused) {
        st.reused++;
      } else {
        rec = {};
        TagInfo tags;
        const uint32_t tagStart = micros();
        bool opened = false;
        if (srec.cluster && card.openCluster(srec.cluster, srec.size)) {
          opened = true;
          st.fastOpens++;
        } else if (card.open(path + "/" + name)) {
          opened = true;
          st.pathOpens++;
        } else {
          st.failedOpens++;
        }
        if (opened) {
          readTags(card, tags);
          card.close();
        }
        tagUs += micros() - tagStart;
        libFillSong(rec, tags, name.c_str(), path.c_str());
        st.tagged++;
      }
      rec.folderId = f;
      rec.nameOffset = srec.nameOffset;
      rec.nameLen = srec.nameLen;
      rec.fileSize = srec.size;
      rec.mtime = srec.mtime;
      rec.artistId = rec.albumId = LIB_NONE;
      ok = songs.append(&rec);

      const uint32_t done = songs.count();
      if (ok && done % CHECKPOINT_EVERY == 0) {
        Checkpoint next = {};
        next.listingSum = w.fp.sum;
        next.listingSum2 = w.fp.sum2;
        next.songCount = w.songs;
        next.done = done;
        next.songsCrc = songs.crc();
        next.oldGen = oldGen;
        ok = songs.sync() && writeCheckpoint(next);
      }
      if (done % 8 == 0) {
        report(o, "Reading tags", done, w.songs, path.c_str());
      }
      if (o.stopAfterSongs && done >= o.stopAfterSongs) {
        st.error = "stopped by test hook";
        ok = false;
      }
    }
    if (folderSame) {
      patch.keep(oldFirst, frec.firstSong, frec.songCount);
    }
  }
  st.tagMs = tagUs / 1000;
  f_close(checkpointFil);
  cardDeleteFil(checkpointFil);
  checkpointFil = nullptr;
  if (!ok) {
    if (st.error.isEmpty()) {
      st.error = "songs: write failed";
    }
    return false;
  }
  if (!songs.finish()) {
    st.error = "songs: finish failed";
    return false;
  }
  return true;
}

// ================================================================ Phase C: sort
static void titleKey(SortRec &k, const SongRecord &r, uint32_t id) {
  memset(&k, 0, sizeof(k));
  k.key[0] = libBucket(r.title);
  lowerCopy(k.key + 1, 39, r.title);
  putBe32(k.key + 40, id);
  k.id = id;
}

static uint32_t fnv(const char *s) {
  return fnv1a(s, strlen(s), false);
}

// Copies the first n bytes of a normalized name (LibClean), zero-padded.
static void normCopy(uint8_t *dst, size_t n, const String &norm) {
  memset(dst, 0, n);
  memcpy(dst, norm.c_str(), std::min(n, norm.length()));
}

// The name an artist key groups by: album artist, else track artist;
// "Various Artists" for a compilation.
static const char *groupArtist(const SongRecord &r, bool various) {
  return various ? kVarious : (r.albumArtist[0] ? r.albumArtist : r.artist);
}

// Stage 1 of the artist order (see regroup()):
// artist(20: bucket, normalized name prefix, normalized name hash) |
// album(16: normalized prefix, hash) | disc | track | title's first letter |
// hash of the artist name as spelled. Names are normalized (LibClean) so
// "KoRn" and "Korn", "The Prodigy" and "Prodigy" group as one artist, and
// "X" with its discs as one album. Equal first 20 bytes = same artist;
// equal first 36 = same album.
static void artistKey(SortRec &k, const SongRecord &r, uint32_t id, bool various) {
  memset(&k, 0, sizeof(k));
  const char *name = groupArtist(r, various);
  if (name[0]) {
    k.key[0] = libBucket(stripThe(name));
    normCopy(k.key + 1, 15, libNormName(name, true));
  } else {
    k.key[0] = 0xFF;  // unknown artist sorts last
  }
  putBe32(k.key + 16, name[0] ? libNormHash(name, true) : 0);
  if (r.album[0]) {
    normCopy(k.key + 20, 12, libNormName(r.album, false));
    putBe32(k.key + 32, libNormHash(r.album, false));
  } else {
    memset(k.key + 20, 0xFF, 16);  // no album: the artist's "none" album
  }
  k.key[36] = r.disc;
  putBe16(k.key + 37, r.track);
  k.key[39] = tolower(static_cast<unsigned char>(r.title[0]));
  putBe32(k.key + 40, fnv(name));
  k.id = id;
}

static void albumKey(SortRec &k, const AlbumRecord &album, const char *artistName, uint32_t albumId,
                     bool unknown) {
  memset(&k, 0, sizeof(k));
  k.key[0] = unknown ? 0xFF : libBucket(album.name);
  lowerCopy(k.key + 1, 23, album.name);
  putBe32(k.key + 24, hashLower(album.name));
  lowerCopy(k.key + 28, 12, stripThe(artistName));
  putBe32(k.key + 40, albumId);
  k.id = albumId;
}

// Songs of one folder sharing an album name, with no album artist, by two or
// more different track artists: a compilation, filed under "Various Artists".
struct AlbumArtists {
  uint32_t album;
  uint32_t artist;
  bool various;
};

// patch: if set, only songs of changed folders are fed (the rest come from
// the previous build's sorted keys).
static bool feedSorters(uint32_t gen, const IndexOptions &o, ExtSort &titles, ExtSort &artists,
                        IndexStats &st, const PatchInfo *patch) {
  LibReader folders, songs;
  if (!folders.open(gen, LF_FOLDERS, sizeof(FolderRecord)) ||
      !songs.open(gen, LF_SONGS, sizeof(SongRecord))) {
    st.error = "sort: cannot open songs";
    return false;
  }
  folders.setReadAhead(2048);
  songs.setReadAhead(8192);  // each folder is read twice: prefetched once, then from RAM
  uint32_t readUs = 0, addUs = 0, t0;
  std::vector<AlbumArtists> albums;
  FolderRecord f;
  SongRecord r;
  SortRec k;
  for (uint32_t fi = 0; fi < folders.count(); fi++) {
    folders.read(fi, &f);
    if (f.songCount == 0 || (patch && patch->isKeptNew(f.firstSong))) {
      continue;
    }
    songs.prefetch(f.firstSong, f.songCount);
    // Pass 1: find compilations in this folder.
    albums.clear();
    for (uint32_t s = f.firstSong; s < f.firstSong + f.songCount; s++) {
      t0 = micros();
      songs.read(s, &r);
      readUs += micros() - t0;
      if (!r.album[0] || r.albumArtist[0]) {
        continue;
      }
      const uint32_t album = libNormHash(r.album, false);
      const uint32_t artist = libNormHash(r.artist, true);
      auto it = std::find_if(albums.begin(), albums.end(),
                             [&](const AlbumArtists &a) { return a.album == album; });
      if (it == albums.end()) {
        albums.push_back({album, artist, false});
      } else if (it->artist != artist) {
        it->various = true;
      }
    }
    // Pass 2: sort keys.
    for (uint32_t s = f.firstSong; s < f.firstSong + f.songCount; s++) {
      t0 = micros();
      songs.read(s, &r);
      readUs += micros() - t0;
      bool various = false;
      if (r.album[0] && !r.albumArtist[0]) {
        const uint32_t album = libNormHash(r.album, false);
        for (const AlbumArtists &a : albums) {
          if (a.album == album) {
            various = a.various;
            break;
          }
        }
      }
      titleKey(k, r, s);
      t0 = micros();
      if (!titles.add(k)) {
        st.error = "sort: title sorter failed: " + titles.error();
        return false;
      }
      addUs += micros() - t0;
      artistKey(k, r, s, various);
      t0 = micros();
      if (!artists.add(k)) {
        st.error = "sort: artist sorter failed: " + artists.error();
        return false;
      }
      addUs += micros() - t0;
      if (s % 64 == 0) {
        report(o, "Sorting", s, songs.count());
      }
    }
  }
  st.feedReadMs = readUs / 1000;
  st.feedAddMs = addUs / 1000;
  return titles.finish() && artists.finish();
}

static bool isFlagged(const std::vector<uint8_t> &bits, uint32_t i) {
  return i / 8 < bits.size() && (bits[i / 8] >> (i % 8)) & 1;
}
static void setFlag(std::vector<uint8_t> &bits, uint32_t i) {
  if (i / 8 < bits.size()) bits[i / 8] |= 1 << (i % 8);
}

// Also saves the sorted records as title.keys for the next build (all of
// them: the next build's patch needs every song).
//
// Duplicates: copies of a song have the same title, so they come together in
// title order. Within a run of equal title keys, songs with the same title
// and artist (normalized) and lengths within 2 s are one song; the copy kept
// is the one in the folder with the most songs (most likely its album), then
// one with an album and a track number, then the first. The others are
// marked in `dup` and left out of every index.
static bool writeTitleIndex(uint32_t gen, KeyStream &titles, IndexStats &st, std::vector<uint8_t> &dup) {
  LibWriter idx, keys;
  LibReader songs, folders;
  JumpBuilder jump;
  if (!idx.create(gen, LF_TITLE_INDEX, sizeof(uint32_t), sizeof(JumpTable)) ||
      !keys.create(gen, LF_TITLE_KEYS, sizeof(SortRec)) || !songs.open(gen, LF_SONGS, sizeof(SongRecord)) ||
      !folders.open(gen, LF_FOLDERS, sizeof(FolderRecord))) {
    return false;
  }
  struct Copy {
    uint32_t id, title, artist, ms, score;
  };
  std::vector<SortRec> run;
  std::vector<Copy> copies;
  std::vector<uint32_t> kept;
  auto flush = [&]() -> bool {
    if (run.size() > 1) {
      copies.clear();
      SongRecord r;
      FolderRecord f;
      for (const SortRec &k : run) {
        if (!songs.read(k.id, &r)) return false;
        const uint32_t inFolder = folders.read(r.folderId, &f) ? std::min<uint32_t>(f.songCount, 0xFFFF) : 0;
        copies.push_back({k.id, libNormHash(r.title, true), libNormHash(r.artist, true), r.durationMs,
                          inFolder << 2 | (r.album[0] ? 2u : 0u) | (r.track ? 1u : 0u)});
      }
      std::stable_sort(copies.begin(), copies.end(), [](const Copy &a, const Copy &b) { return a.score > b.score; });
      kept.clear();
      for (size_t i = 0; i < copies.size(); i++) {
        bool same = false;
        for (uint32_t j : kept) {
          const Copy &a = copies[i], &b = copies[j];
          const uint32_t diff = a.ms > b.ms ? a.ms - b.ms : b.ms - a.ms;
          same = same || (a.title == b.title && a.artist == b.artist && diff <= 2000);
        }
        if (same) {
          setFlag(dup, copies[i].id);
          st.duplicates++;
        } else {
          kept.push_back(i);
        }
      }
    }
    for (const SortRec &k : run) {
      if (isFlagged(dup, k.id)) continue;
      jump.add(k.key[0], idx.count());
      if (!idx.append(&k.id)) return false;
    }
    run.clear();
    return true;
  };
  SortRec k;
  while (titles.next(k)) {
    if (!keys.append(&k)) return false;
    if (!run.empty() && memcmp(run[0].key, k.key, 40) != 0 && !flush()) return false;  // 40: bucket + title
    run.push_back(k);
  }
  if (!flush()) return false;
  jump.finish(idx.count());
  return idx.finish(&jump.table) && keys.finish();
}

// Stage 2 of the artist order. Reads the stage 1 keys in order (an artist's
// songs together, each album's together), saves them as artist.keys for the
// next build's patch, and passes every song that isn't a duplicate on to
// `out`, the final order:
// - songs of an album with more than NONE_MAX_SONGS songs keep their key;
// - songs of smaller albums and without an album get key[20] = 0xFF and
//   their title after it: the artist's "none" album, last, by title.
// Also picks each artist's name as spelled most often (artistRep: one song
// id per artist, in order).
static bool regroup(uint32_t gen, KeyStream &in, ExtSort &out, const std::vector<uint8_t> &dup,
                    std::vector<uint32_t> &artistRep) {
  LibWriter keys;
  LibReader songs;
  if (!keys.create(gen, LF_ARTIST_KEYS, sizeof(SortRec)) || !songs.open(gen, LF_SONGS, sizeof(SongRecord))) {
    return false;
  }
  struct Spelling {
    uint32_t hash, count, id;
  };
  std::vector<Spelling> spellings;
  SortRec held[NONE_MAX_SONGS + 1];
  int heldCount = 0;
  bool bigAlbum = false;
  uint8_t curArtist[20], curAlbum[36];
  bool have = false, ok = true;
  SongRecord r;
  auto toNone = [&](SortRec k) {
    if (!songs.read(k.id, &r)) return false;
    k.key[20] = 0xFF;
    lowerCopy(k.key + 21, sizeof(k.key) - 21, r.title);
    return out.add(k);
  };
  auto flushAlbum = [&]() {
    for (int i = 0; i < heldCount; i++) ok = ok && toNone(held[i]);
    heldCount = 0;
    bigAlbum = false;
  };
  auto flushArtist = [&]() {
    const Spelling *best = &spellings[0];
    for (const Spelling &s : spellings) {
      if (s.count > best->count) best = &s;
    }
    artistRep.push_back(best->id);
    spellings.clear();
  };
  SortRec k;
  while (ok && in.next(k)) {
    ok = keys.append(&k);
    if (isFlagged(dup, k.id)) continue;
    const bool newArtist = !have || memcmp(k.key, curArtist, sizeof(curArtist)) != 0;
    const bool newAlbum = newArtist || memcmp(k.key, curAlbum, sizeof(curAlbum)) != 0;
    if (have && newAlbum) flushAlbum();
    if (have && newArtist) flushArtist();
    memcpy(curArtist, k.key, sizeof(curArtist));
    memcpy(curAlbum, k.key, sizeof(curAlbum));
    have = true;
    const uint32_t spelling = getBe32(k.key + 40);
    auto it = std::find_if(spellings.begin(), spellings.end(), [&](const Spelling &s) { return s.hash == spelling; });
    if (it == spellings.end()) spellings.push_back({spelling, 1, k.id});
    else it->count++;
    if (k.key[20] == 0xFF) {  // no album
      ok = ok && toNone(k);
    } else if (bigAlbum) {
      ok = ok && out.add(k);
    } else {
      held[heldCount++] = k;
      if (heldCount > NONE_MAX_SONGS) {
        bigAlbum = true;
        for (int i = 0; i < heldCount; i++) ok = ok && out.add(held[i]);
        heldCount = 0;
      }
    }
  }
  if (have) {
    flushAlbum();
    flushArtist();
  }
  return ok && keys.finish() && out.finish();
}

// Artists, albums and artist.idx from the final artist order (regroup()).
// Marks the songs of "none" albums in noneBits, and adds the Albums tab's
// "none" album (over none.idx) at the end.
static bool writeArtists(uint32_t gen, KeyStream &artistSort, ExtSort &albumSort, IndexStats &st,
                         const std::vector<uint32_t> &artistRep, std::vector<uint8_t> &noneBits) {
  LibReader songs;
  LibWriter artists, albums, artistIdx;
  if (!songs.open(gen, LF_SONGS, sizeof(SongRecord)) ||
      !artists.create(gen, LF_ARTISTS, sizeof(ArtistRecord), sizeof(JumpTable)) ||
      !albums.create(gen, LF_ALBUMS, sizeof(AlbumRecord)) ||
      !artistIdx.create(gen, LF_ARTIST_INDEX, sizeof(uint32_t))) {
    return false;
  }
  const uint32_t variousHash = libNormHash(kVarious, true);
  JumpBuilder jump;
  ArtistRecord artist = {};
  AlbumRecord album = {};
  uint8_t curArtist[20];
  uint8_t curAlbum[36];
  bool have = false;
  bool ok = true;
  SortRec k;
  SongRecord r;

  auto flushAlbum = [&]() {
    SortRec ak;
    albumKey(ak, album, artist.name, albums.count(), false);
    // An artist's "none" album is on their page, not in the Albums tab.
    ok = ok && ((album.flags & ALBUM_NONE) || albumSort.add(ak)) && albums.append(&album);
    artist.albumCount++;
  };
  auto isNone = [](const uint8_t *key) { return key[20] == 0xFF; };
  auto flushArtist = [&]() { ok = ok && artists.append(&artist); };

  uint32_t pos = 0;
  while (ok && artistSort.next(k)) {
    const bool newArtist = !have || memcmp(k.key, curArtist, sizeof(curArtist)) != 0;
    // A "none" album's keys differ after byte 20 (titles): one album.
    const bool newAlbum = newArtist || (!(isNone(k.key) && isNone(curAlbum)) &&
                                        memcmp(k.key, curAlbum, sizeof(curAlbum)) != 0);
    if (have && newAlbum) {
      flushAlbum();
    }
    if (have && newArtist) {
      flushArtist();
    }
    if (newArtist || newAlbum) {
      songs.read(k.id, &r);
    }
    if (newArtist) {
      artist = {};
      // The name as spelled most often (regroup()).
      SongRecord rep;
      const bool haveRep = artists.count() < artistRep.size() && songs.read(artistRep[artists.count()], &rep);
      const SongRecord &named = haveRep ? rep : r;
      const char *name = getBe32(k.key + 16) == variousHash && k.key[0] != 0xFF
                           ? kVarious
                           : (named.albumArtist[0] ? named.albumArtist : named.artist);
      libCopyField(artist.name, sizeof(artist.name), name[0] ? String(name) : String(kUnknownArtist));
      artist.firstAlbum = albums.count();
      artist.firstSongPos = pos;
      jump.add(k.key[0], artists.count());
      memcpy(curArtist, k.key, sizeof(curArtist));
    }
    if (newAlbum) {
      album = {};
      if (isNone(k.key)) {
        libCopyField(album.name, sizeof(album.name), NONE_ALBUM_NAME);
        album.flags = ALBUM_NONE;
      } else {
        libCopyField(album.name, sizeof(album.name), String(r.album));
        album.year = r.year;
      }
      album.artistId = artists.count();  // id the current artist will get
      album.firstSongPos = pos;
      memcpy(curAlbum, k.key, sizeof(curAlbum));
    }
    if (isNone(k.key)) {
      setFlag(noneBits, k.id);
      st.noneSongs++;
    }
    ok = ok && artistIdx.append(&k.id);
    artist.songCount++;
    album.songCount++;
    pos++;
    have = true;
  }
  if (have) {
    flushAlbum();
    flushArtist();
  }
  if (ok && st.noneSongs) {  // the Albums tab's "none": every artist's, by title (none.idx)
    album = {};
    libCopyField(album.name, sizeof(album.name), NONE_ALBUM_NAME);
    album.artistId = LIB_NONE;
    album.flags = ALBUM_NONE_ALL;
    album.songCount = st.noneSongs;
    SortRec ak;
    albumKey(ak, album, "", albums.count(), false);
    ok = albumSort.add(ak) && albums.append(&album);
  }
  jump.finish(artists.count());
  st.artists = artists.count();
  st.albums = albums.count();
  return ok && artists.finish(&jump.table) && albums.finish() && artistIdx.finish() && albumSort.finish();
}

// none.idx: the songs marked in noneBits, in title order.
static bool writeNoneIndex(uint32_t gen, const std::vector<uint8_t> &noneBits) {
  LibReader titles;
  LibWriter idx;
  if (!titles.open(gen, LF_TITLE_INDEX, sizeof(uint32_t)) || !idx.create(gen, LF_NONE_INDEX, sizeof(uint32_t))) {
    return false;
  }
  titles.setReadAhead(4096);
  uint32_t id;
  for (uint32_t pos = 0; pos < titles.count(); pos++) {
    if (!titles.read(pos, &id)) return false;
    if (isFlagged(noneBits, id) && !idx.append(&id)) return false;
  }
  return idx.finish();
}

static bool writeAlbumIndex(uint32_t gen, ExtSort &albumSort, IndexStats &st) {
  LibWriter idx;
  JumpBuilder jump;
  SortRec k;
  if (!idx.create(gen, LF_ALBUM_INDEX, sizeof(uint32_t), sizeof(JumpTable))) {
    return false;
  }
  while (albumSort.next(k)) {
    jump.add(k.key[0], idx.count());
    if (!idx.append(&k.id)) {
      return false;
    }
  }
  jump.finish(idx.count());
  st.sortRuns += albumSort.runCount();
  return idx.finish(&jump.table);
}

// Bigger buffers mean fewer runs to merge (merge reads per run shrink with
// the square of the buffer size), but the player needs memory too. Both
// sorters get the same size, the largest that leaves HEAP_RESERVE in the
// largest free block after both are allocated; no more than `records` need
// (patch mode sorts only the changed songs).
static bool beginSorters(ExtSort &a, const String &pathA, ExtSort &b, const String &pathB,
                         uint32_t records, IndexStats &st) {
  const uint32_t want = std::max<uint32_t>(
    SORT_BUFFER_MIN, std::min<uint32_t>(SORT_BUFFER_MAX, (records + 16) * sizeof(SortRec)));
  for (uint32_t bytes = want; bytes >= SORT_BUFFER_MIN; bytes = bytes > 4096 ? bytes - 4096 : 0) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < bytes + HEAP_RESERVE ||
        !a.begin(pathA, bytes, records)) {
      continue;
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= bytes + HEAP_RESERVE &&
        b.begin(pathB, bytes, records)) {
      if (ESP.getFreeHeap() >= SORT_FREE_AFTER || bytes == SORT_BUFFER_MIN) {
        st.sortBufferBytes = bytes;
        return true;
      }
      b.end();
    }
    a.end();
  }
  st.sortBufferBytes = SORT_BUFFER_MIN;  // last try, even below the reserve
  return a.begin(pathA, SORT_BUFFER_MIN, records) && b.begin(pathB, SORT_BUFFER_MIN, records);
}

// Patch mode: the previous build's sorted keys, minus songs of changed
// folders, with ids moved to their new numbers, merged with the freshly
// sorted keys of the changed folders' songs. Reads the old keys in order (no
// random access). Kept songs' keys are unchanged and the id shift preserves
// their order; next() still checks the output is in order and fails if not.
class KeysMerge : public KeyStream {
public:
  KeysMerge(LibReader &old, const PatchInfo &patch, ExtSort &fresh, bool idInKey)
      : old_(old), patch_(patch), fresh_(fresh), idInKey_(idInKey) {}
  bool next(SortRec &out) override {
    if (!primed_) {
      primed_ = true;
      haveOld_ = nextOld();
      haveNew_ = fresh_.next(new_);
    }
    if (!haveOld_ && !haveNew_) {
      return false;
    }
    if (haveOld_ && (!haveNew_ || sortRecLess(old_rec_, new_))) {
      out = old_rec_;
      haveOld_ = nextOld();
    } else {
      out = new_;
      haveNew_ = fresh_.next(new_);
    }
    if (emitted_ > 0 && sortRecLess(out, last_)) {
      failed_ = true;
      return false;
    }
    last_ = out;
    emitted_++;
    return true;
  }
  bool failed() const { return failed_; }
  uint32_t emitted() const { return emitted_; }

private:
  bool nextOld() {
    while (pos_ < old_.count()) {
      if (!old_.read(pos_++, &old_rec_)) {
        failed_ = true;
        return false;
      }
      const uint32_t id = patch_.newId(old_rec_.id);
      if (id == LIB_NONE) {
        continue;  // its folder changed: it comes from `fresh` if still there
      }
      old_rec_.id = id;
      if (idInKey_) {  // title keys end with the id (equal titles in id order)
        old_rec_.key[40] = id >> 24;
        old_rec_.key[41] = id >> 16;
        old_rec_.key[42] = id >> 8;
        old_rec_.key[43] = id;
      }
      return true;
    }
    return false;
  }
  LibReader &old_;
  const PatchInfo &patch_;
  ExtSort &fresh_;
  const bool idInKey_;
  SortRec old_rec_, new_, last_;
  bool primed_ = false, haveOld_ = false, haveNew_ = false, failed_ = false;
  uint32_t pos_ = 0, emitted_ = 0;
};

// Opens and checks (data CRC) one of the previous build's key files.
static bool openOldKeys(LibReader &reader, uint32_t oldGen, LibFileType type, uint32_t oldSongs) {
  if (!reader.open(oldGen, type, sizeof(SortRec)) || reader.count() != oldSongs ||
      !reader.verify()) {
    reader.close();
    return false;
  }
  reader.setReadAhead(4096);
  return true;
}

// patch: non-null to merge with the previous build's keys (see KeysMerge).
static bool sortAndGroup(uint32_t gen, const IndexOptions &o, IndexStats &st, uint32_t oldGen,
                         uint32_t oldSongs, const PatchInfo *patch) {
  ExtSort titles, artists, albums;
  LibReader oldTitleKeys, oldArtistKeys;
  if (patch && !(openOldKeys(oldTitleKeys, oldGen, LF_TITLE_KEYS, oldSongs) &&
                 openOldKeys(oldArtistKeys, oldGen, LF_ARTIST_KEYS, oldSongs))) {
    st.error = "previous sort keys missing or invalid";
    return false;  // caller falls back to a full sort
  }
  const uint32_t toSort = patch ? st.songs - patch->keptSongs : st.songs;
  st.largestBlockAtSort = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (!beginSorters(titles, tempPath(gen, "sort-title.tmp"), artists,
                    tempPath(gen, "sort-artist.tmp"), toSort, st)) {
    st.error = "sort: out of memory";
    return false;
  }
  static const PatchInfo noPatch;
  const PatchInfo &kept = patch ? *patch : noPatch;
  uint32_t t = millis();
  if (!feedSorters(gen, o, titles, artists, st, patch)) {
    return false;
  }
  st.feedMs = millis() - t;
  report(o, "Writing indexes", 0, 0);
  t = millis();
  std::vector<uint8_t> dup((st.songs + 7) / 8), noneBits((st.songs + 7) / 8);
  std::vector<uint32_t> artistRep;
  st.duplicates = st.noneSongs = 0;
  KeysMerge titleMerge(oldTitleKeys, kept, titles, true);
  if (patch ? !writeTitleIndex(gen, titleMerge, st, dup) || titleMerge.failed() ||
                titleMerge.emitted() != st.songs
            : !writeTitleIndex(gen, titles, st, dup)) {
    st.error = "sort: title index failed";
    return false;
  }
  st.sortRuns += titles.runCount();
  titles.end();  // free its buffer before the album sorter needs one
  oldTitleKeys.close();
  st.titleIdxMs = millis() - t;
  t = millis();
  // Stage 2 (regroup) into a third sorter, in the title sorter's memory.
  ExtSort grouped;
  KeysMerge artistMerge(oldArtistKeys, kept, artists, false);
  if (!grouped.begin(tempPath(gen, "sort-group.tmp"), st.sortBufferBytes, st.songs) ||
      (patch ? !regroup(gen, artistMerge, grouped, dup, artistRep) || artistMerge.failed() ||
                 artistMerge.emitted() != st.songs
             : !regroup(gen, artists, grouped, dup, artistRep))) {
    st.error = "sort: grouping failed";
    return false;
  }
  st.sortRuns += artists.runCount();
  artists.end();
  oldArtistKeys.close();
  if (!albums.begin(tempPath(gen, "sort-album.tmp"), ALBUM_SORT_BUFFER) ||
      !writeArtists(gen, grouped, albums, st, artistRep, noneBits)) {
    st.error = "sort: artist index failed";
    return false;
  }
  st.sortRuns += grouped.runCount();
  grouped.end();
  st.artistIdxMs = millis() - t;
  t = millis();
  if (!writeAlbumIndex(gen, albums, st)) {
    st.error = "sort: album index failed";
    return false;
  }
  if (!writeNoneIndex(gen, noneBits)) {
    st.error = "sort: none index failed";
    return false;
  }
  st.albumIdxMs = millis() - t;
  st.sortPatched = patch != nullptr;
  return true;
}

// ================================================================ Check
// Read-only depth-first listing: same folders and files as the walk, no
// writes, no sorting, no per-entry allocation.
static void checkFolder(const IndexOptions &o, char *path, size_t len, uint16_t depth,
                        uint32_t cluster, Fingerprint &fp) {
  CardDir dir;
  const String fatfsPath = String(CARD_DRIVE) + (len ? path : "/");
  if (!dir.open(fatfsPath.c_str(), cluster)) {
    return;
  }
  static FILINFO info;  // refilled by each read; names are copied into path first
  while (dir.read(info)) {
    if (info.fattrib & (AM_HID | AM_SYS)) {
      continue;
    }
    const size_t nameLen = strlen(info.fname);
    if (info.fattrib & AM_DIR) {
      const bool isBench = strcmp(info.fname, "_bench") == 0;
      if (depth >= MAX_DEPTH || (isBench ? !o.includeBench : librarySkipFolder(info.fname)) ||
          len + 1 + nameLen > MAX_PATH_BYTES) {
        continue;
      }
      uint32_t sub = 0;
      cardCaptureDir(dir.dir(), info, sub);
      path[len] = '/';
      memcpy(path + len + 1, info.fname, nameLen + 1);
      checkFolder(o, path, len + 1 + nameLen, depth + 1, sub, fp);
      path[len] = 0;
    } else if (libraryIsMp3Name(info.fname)) {
      fp.add(path, len, info.fname, nameLen, static_cast<uint32_t>(info.fsize),
             (static_cast<uint32_t>(info.fdate) << 16) | info.ftime);
      if (fp.count % 100 == 0) {
        report(o, "Checking card", fp.count, 0, len ? path : "/");
      }
    }
  }
  dir.close();
}

static void quickFingerprint(const IndexOptions &o, Fingerprint &fp) {
  static char path[MAX_PATH_BYTES + 2];
  path[0] = 0;
  checkFolder(o, path, 0, 0, 0, fp);
}

// ================================================================ Driver
bool indexLibrary(const IndexOptions &o, IndexStats &st) {
  st = IndexStats();
  const uint32_t t0 = millis();
  ManifestRecord live = {};
  const uint32_t liveGen = LibStore::findCurrent(&live);
  const uint32_t oldGen = o.forceFull ? LIB_NONE : liveGen;

  // 0: unchanged since the live library was built?
  if (!o.forceFull && !o.fullSort && liveGen != LIB_NONE) {
    report(o, "Checking card", 0, 0);
    Fingerprint fp;
    quickFingerprint(o, fp);
    st.checkMs = millis() - t0;
    if (fp.sum == live.listingSum && fp.sum2 == live.listingSum2 && fp.count == live.songCount) {
      if (o.removeOld) {
        LibStore::removeAllExcept(liveGen);  // drop any abandoned builds
      }
      st.unchanged = true;
      st.ok = true;
      st.gen = liveGen;
      st.songs = live.songCount;
      st.folders = live.folderCount;
      st.artists = live.artistCount;
      st.albums = live.albumCount;
      return true;
    }
  }

  // Continue an unfinished build newer than the live library, if it left a
  // checkpoint; otherwise start a fresh generation.
  uint32_t gen = LIB_NONE;
  const uint32_t newest = LibStore::newestGeneration();
  Checkpoint cp;
  if (newest != LIB_NONE && newest != liveGen && (liveGen == LIB_NONE || newest > liveGen) &&
      readCheckpoint(newest, cp)) {
    gen = newest;
  }
  LibStore::removeAllExcept(liveGen, gen);
  if (gen == LIB_NONE) {
    gen = LibStore::newGeneration();
  }
  if (gen == LIB_NONE) {
    st.error = "cannot create a generation folder";
    return false;
  }

  // A: walk.
  report(o, "Scanning folders", 0, 0);
  WalkResult w;
  if (!walk(gen, o, w, st)) {
    return false;
  }
  st.walkMs = millis() - t0 - st.checkMs;
  st.folders = w.folders;
  st.songs = w.songs;

  // B: songs.
  uint32_t t = millis();
  PatchInfo patch;
  if (!buildSongs(gen, oldGen, w, o, st, patch)) {
    return false;
  }
  st.songsMs = millis() - t;
  f_unlink(cardPath(LibStore::filePath(gen, LF_TEMP_SKELETON)).c_str());

  // C: sort and group.
  t = millis();
  // Merge into the previous build's sorted keys when possible; otherwise (no
  // usable keys, resumed build, reordered folders) sort everything.
  st.keptSongs = patch.keptSongs;
  bool sorted = false;
  // The patch reads all of the old keys: worth it only when most songs are
  // kept (a library that shrank a lot sorts its few songs faster).
  if (!patch.usable) {
    st.patchSkipped = oldGen == LIB_NONE ? "no previous library"
                      : patch.why[0]   ? patch.why
                      : st.resumed     ? "resumed build"
                                       : "not applicable";
  } else if (o.fullSort) {
    st.patchSkipped = "full sort requested";
  } else if (patch.keptSongs * 2 < live.songCount) {
    st.patchSkipped = "fewer than half the songs kept";
  } else {
    sorted = sortAndGroup(gen, o, st, oldGen, live.songCount, &patch);
    if (!sorted) {
      st.patchSkipped = "patch failed: " + st.error + " (free heap " + ESP.getFreeHeap() + ")";
      st.sortRuns = 0;
      st.error = "";
    }
  }
  patch.drop();  // free the ranges before a full sort needs its buffers
  if (!sorted && !sortAndGroup(gen, o, st, LIB_NONE, 0, nullptr)) {
    return false;
  }
  st.sortMs = millis() - t;

  // D: commit.
  t = millis();
  report(o, "Saving", 0, 0);
  f_unlink(cardPath(tempPath(gen, "progress.tmp")).c_str());
  ManifestRecord m = {};
  m.songCount = st.songs;
  m.folderCount = st.folders;
  m.artistCount = st.artists;
  m.albumCount = st.albums;
  m.nameBytes = w.nameBytes;
  m.listingSum = w.fp.sum;
  m.listingSum2 = w.fp.sum2;
  m.buildMs = millis() - t0;
  if (!LibStore::commit(gen, m)) {
    st.error = "commit: read-back verification failed";
    return false;
  }
  if (o.removeOld) {
    LibStore::removeAllExcept(gen);
  }
  st.commitMs = millis() - t;
  st.gen = gen;
  st.ok = true;
  st.minFreeHeap = ESP.getMinFreeHeap();
  return true;
}
