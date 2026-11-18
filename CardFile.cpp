#include "CardFile.h"
#include <new>

#include <algorithm>

static FATFS *capturedFs = nullptr;
static size_t filBytes = sizeof(FIL);  // what cardNewFil() allocates

void cardNoteMount() {
  FF_DIR dir;
  filBytes = sizeof(FIL);
  if (f_opendir(&dir, CARD_DRIVE "/") == FR_OK) {
    if (dir.obj.fs && dir.obj.fs->ssize == 512) {
      filBytes = offsetof(FIL, buf) + 512;
    }
    f_closedir(&dir);
  }
}

FIL *cardNewFil() {
  return static_cast<FIL *>(calloc(1, filBytes));
}

void cardDeleteFil(FIL *fil) {
  free(fil);
}

static uint16_t le16(const uint8_t *p) {
  return p[0] | (p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint32_t entryCluster(const FATFS *fs, const uint8_t *e) {
  uint32_t cluster = le16(e + 26);
  if (fs->fs_type == FS_FAT32) {
    cluster |= static_cast<uint32_t>(le16(e + 20)) << 16;
  }
  return cluster;
}

static bool captureRaw(const FF_DIR &dir, const FILINFO &info, uint32_t &startCluster);

bool cardCaptureEntry(const FF_DIR &dir, const FILINFO &info, uint32_t &startCluster) {
  return info.fsize != 0 && !(info.fattrib & AM_DIR) && captureRaw(dir, info, startCluster);
}

bool cardCaptureDir(const FF_DIR &dir, const FILINFO &info, uint32_t &startCluster) {
  return (info.fattrib & AM_DIR) && captureRaw(dir, info, startCluster);
}

static bool captureRaw(const FF_DIR &dir, const FILINFO &info, uint32_t &startCluster) {
  FATFS *fs = dir.obj.fs;
  if (!fs) {
    return false;
  }
  const uint32_t sectorSize = fs->ssize;
  // f_readdir advances past the entry (dptr += 32) unless it hit the end of
  // the directory, in which case dptr still points at it. Try both; the
  // content check below decides.
  const uint32_t candidates[2] = {dir.dptr >= 32 ? dir.dptr - 32 : UINT32_MAX, dir.dptr};
  for (uint32_t dptr : candidates) {
    if (dptr == UINT32_MAX) {
      continue;
    }
    const uint8_t *e = fs->win + (dptr % sectorSize);
    if (e[0] == 0 || e[0] == 0xE5 || (e[11] & 0x3F) != info.fattrib ||
        le32(e + 28) != info.fsize || le16(e + 22) != info.ftime || le16(e + 24) != info.fdate) {
      continue;
    }
    const uint32_t cluster = entryCluster(fs, e);
    if (cluster < 2 || cluster >= fs->n_fatent) {
      continue;
    }
    startCluster = cluster;
    capturedFs = fs;
    return true;
  }
  return false;
}

bool CardFile::openCluster(uint32_t startCluster, uint32_t size) {
  close();
  if (!capturedFs || startCluster < 2) {
    return false;
  }
  if (!fil_) {
    fil_ = cardNewFil();
    if (!fil_) {
      return false;
    }
  }
  // Same state f_open leaves a read-only FIL in (FatFs R0.15, FF_FS_LOCK 0).
  memset(fil_, 0, offsetof(FIL, buf));  // the buffer is only read after a sector load
  fil_->obj.fs = capturedFs;
  fil_->obj.id = capturedFs->id;
  fil_->obj.attr = AM_ARC;
  fil_->obj.sclust = startCluster;
  fil_->obj.objsize = size;
  fil_->flag = FA_READ;
  open_ = true;
  return true;
}

static void *yieldTask = nullptr;
static uint32_t lastYield = 0;

void cardSetYieldTask(void *taskHandle) {
  yieldTask = taskHandle;
}

void cardYieldPoint() {
  if (yieldTask && xTaskGetCurrentTaskHandle() == yieldTask && millis() - lastYield >= 50) {
    vTaskDelay(1);
    lastYield = millis();
  }
}

FRESULT cardWrite(FIL *fil, const void *data, UINT len, UINT *written) {
  cardYieldPoint();
  const uint8_t *p = static_cast<const uint8_t *>(data);
  *written = 0;
  while (len > 0) {
    const UINT room = 512 - static_cast<UINT>(fil->fptr % 512);
    const UINT piece = len < room ? len : room;
    UINT n = 0;
    const FRESULT fr = f_write(fil, p, piece, &n);
    *written += n;
    if (fr != FR_OK || n != piece) {
      return fr;
    }
    p += piece;
    len -= piece;
  }
  return FR_OK;
}

// ---------------------------------------------------------------- SeekIndex
void SeekIndex::build(FIL *fil) {
  clear();
  FATFS *fs = fil->obj.fs;
  if (!fs) {
    return;
  }
  clusterBytes_ = static_cast<uint32_t>(fs->csize) * fs->ssize;
  const FSIZE_t size = f_size(fil);
  const uint64_t span = static_cast<uint64_t>(STRIDE) * clusterBytes_;
  if (size <= 2 * span) {
    return;  // small file: plain seeks are cheap
  }
  anchors_.reserve(size / span + 1);
  anchors_.push_back(0);  // index 0 unused (seek from the start)
  // Seeking to a cluster boundary leaves fptr there and clust on the cluster
  // before it (FatFs convention); no data sector is read. One forward walk.
  for (uint64_t pos = span; pos < size; pos += span) {
    if (f_lseek(fil, pos) != FR_OK || fil->fptr != pos) {
      clear();
      break;
    }
    anchors_.push_back(fil->clust);
  }
  f_lseek(fil, 0);
}

FRESULT SeekIndex::seek(FIL *fil, FSIZE_t pos) {
  if (anchors_.empty() || pos == 0) {
    return f_lseek(fil, pos);
  }
  const uint32_t target = (pos - 1) / clusterBytes_;  // cluster holding byte pos-1
  if (fil->fptr > 0) {
    const uint32_t cur = (fil->fptr - 1) / clusterBytes_;
    if (target >= cur && target - cur <= STRIDE) {
      return f_lseek(fil, pos);  // short forward seek: FatFs walks from here
    }
  }
  const uint32_t i = target / STRIDE;
  if (i == 0 || i >= anchors_.size()) {
    return f_lseek(fil, pos);
  }
  fil->fptr = static_cast<FSIZE_t>(i) * STRIDE * clusterBytes_;
  fil->clust = anchors_[i];
  return f_lseek(fil, pos);
}

// ---------------------------------------------------------------- CardDir
uint32_t CardDir::clusterOpens = 0;
uint32_t CardDir::pathOpens = 0;

bool CardDir::open(const char *fatfsPath, uint32_t cluster) {
  close();
  path_ = fatfsPath;
  cluster_ = cluster;
  FATFS *fs = capturedFs;
  if (cluster < 2 || !fs || fs->fs_type != FS_FAT32 || cluster >= fs->n_fatent) {
    return reopenByPath();
  }
  // Same state f_opendir + dir_sdi(dp, 0) leave (FatFs R0.15, FAT32, no lock).
  memset(&dir_, 0, sizeof(dir_));
  dir_.obj.fs = fs;
  dir_.obj.id = fs->id;
  dir_.obj.attr = AM_DIR;
  dir_.obj.sclust = cluster;
  dir_.dptr = 0;
  dir_.clust = cluster;
  dir_.sect = fs->database + static_cast<LBA_t>(fs->csize) * (cluster - 2);
  dir_.dir = fs->win;
  dir_.blk_ofs = 0xFFFFFFFF;
  open_ = true;
  verify_ = true;
  clusterOpens++;
  return true;
}

bool CardDir::reopenByPath() {
  verify_ = false;
  pathOpens++;
  open_ = f_opendir(&dir_, path_) == FR_OK;
  return open_;
}

bool CardDir::read(FILINFO &info) {
  if (!open_) {
    return false;
  }
  cardYieldPoint();
  const LBA_t first = dir_.sect;
  FRESULT fr = f_readdir(&dir_, &info);
  if (verify_) {
    verify_ = false;
    // The first read loaded the folder's first sector, which starts with the
    // "." entry naming the folder's own cluster. Only that sector can match,
    // so a stale window (another task's read) just means a fallback.
    const FATFS *fs = dir_.obj.fs;
    const uint8_t *e = fs->win;
    const bool ok = fr == FR_OK && fs->winsect == first && memcmp(e, ".          ", 11) == 0 &&
                    (e[11] & AM_DIR) && entryCluster(fs, e) == cluster_;
    if (!ok) {
      clusterOpens--;
      if (!reopenByPath()) {
        return false;
      }
      fr = f_readdir(&dir_, &info);
    }
  }
  return fr == FR_OK && info.fname[0];
}

void CardDir::close() {
  if (open_) {
    f_closedir(&dir_);
    open_ = false;
  }
}

String cardPath(const String &path) {
  return String(CARD_DRIVE) + path;
}

bool CardFile::open(const String &path) {
  close();
  if (!fil_) {
    fil_ = cardNewFil();
  }
  open_ = fil_ && f_open(fil_, cardPath(path).c_str(), FA_READ) == FR_OK;
  return open_;
}

uint32_t CardFile::readCalls = 0;
uint32_t CardFile::cardReads = 0;

void CardFile::setCache(uint32_t bytes) {
  if (bytes != ccap_) {
    free(cache_);
    cache_ = bytes ? static_cast<uint8_t *>(malloc(bytes)) : nullptr;
    ccap_ = cache_ ? bytes : 0;
  }
  clen_ = 0;
}

void CardFile::close() {
  clen_ = 0;
  if (open_) {
    f_close(fil_);
    open_ = false;
  }
}

uint32_t CardFile::size() {
  return open_ ? f_size(fil_) : 0;
}

size_t CardFile::readAt(uint32_t pos, void *buf, size_t len) {
  if (!open_) {
    return 0;
  }
  cardYieldPoint();
  readCalls++;
  const uint32_t size = f_size(fil_);
  if (cache_ && pos < size) {
    len = std::min<size_t>(len, size - pos);
    if (pos >= cpos_ && pos + len <= cpos_ + clen_) {
      memcpy(buf, cache_ + (pos - cpos_), len);
      return len;
    }
    const uint32_t start = pos & ~511u;  // sector-aligned: whole sectors, one command
    if (pos + len <= start + ccap_) {
      UINT n = 0;
      const UINT want = std::min<uint32_t>(ccap_, size - start);
      clen_ = 0;
      cardReads++;
      if (f_lseek(fil_, start) != FR_OK || f_read(fil_, cache_, want, &n) != FR_OK) {
        return 0;
      }
      cpos_ = start;
      clen_ = n;
      const size_t got = pos - start < n ? std::min<size_t>(len, n - (pos - start)) : 0;
      memcpy(buf, cache_ + (pos - start), got);
      return got;
    }
  }
  cardReads++;
  if (f_lseek(fil_, pos) != FR_OK) {
    return 0;
  }
  UINT n = 0;
  if (f_read(fil_, buf, len, &n) != FR_OK) {
    return 0;
  }
  return n;
}
