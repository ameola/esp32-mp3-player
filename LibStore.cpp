#include "LibStore.h"

#include "LibClean.h"
#include <new>

#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <algorithm>
#include <vector>

#include "CardFile.h"
#include "Text.h"

const char *const kLibFileNames[LF_TYPE_COUNT] = {
  "",            // unused (0)
  "manifest.dat",
  "names.dat",
  "folders.dat",
  "songs.dat",
  "artists.dat",
  "albums.dat",
  "title.idx",
  "artist.idx",
  "album.idx",
  "none.idx",
};

uint8_t libBucket(const char *text) {
  char first[5] = {};
  memcpy(first, text, strnlen(text, 4));  // one character is enough
  const String folded = foldAscii(first);
  const char c = tolower(static_cast<unsigned char>(folded.length() ? folded[0] : 0));
  return (c >= 'a' && c <= 'z') ? 1 + (c - 'a') : 0;
}

void libSortCopy(uint8_t *dst, size_t n, const char *src) {
  const String folded = foldAscii(src);
  for (size_t i = 0; i < n && i < folded.length(); i++) {
    dst[i] = tolower(static_cast<unsigned char>(folded[i]));
  }
}

void libCopyField(char *dst, size_t size, const String &text) {
  const size_t n = utf8Prefix(text.c_str(), text.length(), size - 1);  // never half a character
  memcpy(dst, text.c_str(), n);
  memset(dst + n, 0, size - n);
}

void libFillSong(SongRecord &rec, const TagInfo &tags, const char *fileName, const char *folderPath) {
  String title = tags.title;
  if (title.isEmpty()) {
    size_t len = strlen(fileName);
    if (len > 4 && strcasecmp(fileName + len - 4, ".mp3") == 0) {
      len -= 4;
    }
    title = decodeUtf8(reinterpret_cast<const uint8_t *>(fileName), len);
  }
  libCopyField(rec.title, sizeof(rec.title), title);
  libCopyField(rec.artist, sizeof(rec.artist), tags.artist);
  libCopyField(rec.album, sizeof(rec.album), tags.album);
  libCopyField(rec.albumArtist, sizeof(rec.albumArtist), tags.albumArtist);
  rec.track = tags.track;
  rec.disc = tags.disc;
  rec.year = tags.year;
  rec.durationMs = tags.durationMs;
  rec.durationSource = tags.durationSource;
  rec.audioStart = tags.audioStart;
  rec.audioEnd = tags.audioEnd;
  libCleanSong(rec, fileName, folderPath);
}

static uint32_t crc32(uint32_t crc, const void *data, size_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t *>(data), len);
}

static uint32_t headerCrc(const LibHeader &h) {
  return crc32(0, &h, offsetof(LibHeader, headerCrc));
}

// ================================================================ LibWriter
bool LibWriter::create(uint32_t gen, LibFileType type, uint16_t recordSize,
                       uint16_t extraSize) {
  abort();
  if (!fil_) {
    fil_ = cardNewFil();  // out of memory fails the build, never aborts
  }
  if (!fil_ || f_open(fil_, cardPath(LibStore::filePath(gen, type)).c_str(),
                      FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    return false;
  }
  open_ = true;
  if (!wbuf_ && WRITE_BUFFER > 0) {
    wbuf_ = static_cast<uint8_t *>(malloc(WRITE_BUFFER));
  }
  wlen_ = 0;
  header_ = {};
  memcpy(header_.magic, LIB_MAGIC, 4);
  header_.type = type;
  header_.version = LIB_FORMAT_VERSION;
  header_.recordSize = recordSize;
  header_.generation = gen;
  header_.extraSize = extraSize;
  crc_ = 0;
  // Placeholder header (all zero, so it never validates until finish()).
  const LibHeader blank = {};
  UINT written = 0;
  ok_ = f_write(fil_, &blank, sizeof(blank), &written) == FR_OK && written == sizeof(blank);
  return ok_;
}

bool LibWriter::append(const void *records, uint32_t n) {
  cardYieldPoint();
  if (!open_ || !ok_) {
    fr_ = fr_ ? fr_ : 102;  // appended to a closed or failed file
    return false;
  }
  const UINT bytes = n * header_.recordSize;
  if (wbuf_ && bytes <= WRITE_BUFFER) {
    if (wlen_ + bytes > WRITE_BUFFER && !flush()) {
      return false;
    }
    memcpy(wbuf_ + wlen_, records, bytes);
    wlen_ += bytes;
  } else {
    UINT written = 0;
    ok_ = flush() && cardWrite(fil_, records, bytes, &written) == FR_OK && written == bytes;
    if (!ok_) {
      fr_ = fr_ ? fr_ : 101;  // unbuffered write failed
      return false;
    }
  }
  crc_ = crc32(crc_, records, bytes);
  header_.recordCount += n;
  return true;
}

bool LibWriter::flush() {
  if (wlen_ > 0 && ok_) {
    UINT written = 0;
    const FRESULT fr = cardWrite(fil_, wbuf_, wlen_, &written);
    ok_ = fr == FR_OK && written == wlen_;
    if (!ok_) {
      fr_ = fr != FR_OK ? fr : 100;  // 100: short write (card full)
    }
  }
  wlen_ = 0;
  return ok_;
}

bool LibWriter::finish(const void *extra) {
  if (!open_ || !ok_) {
    abort();
    return false;
  }
  UINT written = 0;
  flush();
  if (header_.extraSize > 0) {
    ok_ = ok_ && extra && f_write(fil_, extra, header_.extraSize, &written) == FR_OK &&
          written == header_.extraSize;
    crc_ = crc32(crc_, extra, header_.extraSize);
  }
  header_.dataCrc = crc_;
  header_.headerCrc = headerCrc(header_);
  ok_ = ok_ && f_lseek(fil_, 0) == FR_OK &&
        f_write(fil_, &header_, sizeof(header_), &written) == FR_OK &&
        written == sizeof(header_);
  ok_ = (f_close(fil_) == FR_OK) && ok_;
  open_ = false;
  return ok_;
}

bool LibWriter::sync() {
  return open_ && flush() && f_sync(fil_) == FR_OK;
}

bool LibWriter::resume(uint32_t gen, LibFileType type, uint16_t recordSize, uint32_t count,
                       uint32_t crc) {
  abort();
  if (!fil_) {
    fil_ = cardNewFil();
  }
  if (!fil_ || f_open(fil_, cardPath(LibStore::filePath(gen, type)).c_str(),
                      FA_OPEN_EXISTING | FA_READ | FA_WRITE) != FR_OK) {
    return false;
  }
  open_ = true;
  if (!wbuf_ && WRITE_BUFFER > 0) {
    wbuf_ = static_cast<uint8_t *>(malloc(WRITE_BUFFER));
  }
  wlen_ = 0;
  const FSIZE_t end = sizeof(LibHeader) + static_cast<FSIZE_t>(count) * recordSize;
  ok_ = f_size(fil_) >= end && f_lseek(fil_, end) == FR_OK && f_truncate(fil_) == FR_OK;
  if (!ok_) {
    abort();
    return false;
  }
  header_ = {};
  memcpy(header_.magic, LIB_MAGIC, 4);
  header_.type = type;
  header_.version = LIB_FORMAT_VERSION;
  header_.recordSize = recordSize;
  header_.generation = gen;
  header_.recordCount = count;
  crc_ = crc;
  return true;
}

void LibWriter::abort() {
  wlen_ = 0;  // unwritten records are dropped: an aborted file is never valid
  if (open_) {
    f_close(fil_);
    open_ = false;
  }
  ok_ = false;
}

// ================================================================ LibReader
bool LibReader::open(uint32_t gen, LibFileType type, uint16_t recordSize) {
  close();
  if (!fil_) {
    fil_ = cardNewFil();
  }
  if (!fil_ || f_open(fil_, cardPath(LibStore::filePath(gen, type)).c_str(), FA_READ) != FR_OK) {
    return false;
  }
  open_ = true;
  UINT n = 0;
  const bool ok =
    f_read(fil_, &header_, sizeof(header_), &n) == FR_OK && n == sizeof(header_) &&
    memcmp(header_.magic, LIB_MAGIC, 4) == 0 && header_.type == type &&
    header_.version == LIB_FORMAT_VERSION && header_.recordSize == recordSize &&
    header_.generation == gen && header_.headerCrc == headerCrc(header_) &&
    f_size(fil_) == sizeof(LibHeader) +
                       static_cast<uint64_t>(header_.recordCount) * recordSize +
                       header_.extraSize;
  if (!ok) {
    close();
    return false;
  }
  seek_.build(fil_);
  return true;
}

bool LibReader::loadToRam() {
  if (whole_) return true;
  if (!open_) return false;
  const uint32_t size = f_size(fil_);
  uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(std::max<uint32_t>(size, 1), MALLOC_CAP_SPIRAM));
  UINT got = 0;
  if (!buf || f_lseek(fil_, 0) != FR_OK || f_read(fil_, buf, size, &got) != FR_OK || got != size) {
    free(buf);
    return false;
  }
  free(rbuf_);
  rbuf_ = buf;
  rcap_ = rlen_ = size;
  rpos_ = 0;
  whole_ = true;
  f_close(fil_);  // every read is served from RAM now
  cardDeleteFil(fil_);
  fil_ = nullptr;
  seek_.clear();
  return true;
}

void LibReader::setReadAhead(uint32_t bytes) {
  if (whole_) return;  // already all in RAM
  if (bytes != rcap_) {
    free(rbuf_);
    rbuf_ = bytes ? static_cast<uint8_t *>(malloc(bytes)) : nullptr;
    rcap_ = rbuf_ ? bytes : 0;
  }
  rlen_ = 0;
}

void LibReader::prefetch(uint32_t index, uint32_t n) {
  if (whole_) return;
  const uint32_t offset = sizeof(LibHeader) + index * header_.recordSize;
  const uint32_t len = n * header_.recordSize;
  if (!rbuf_ || !open_ || n == 0 || len > rcap_ || index + n > header_.recordCount ||
      (offset >= rpos_ && offset + len <= rpos_ + rlen_)) {
    return;
  }
  UINT got = 0;
  const UINT want = std::min<uint32_t>(rcap_, f_size(fil_) - offset);
  rlen_ = 0;
  if (seek_.seek(fil_, offset) == FR_OK && f_read(fil_, rbuf_, want, &got) == FR_OK &&
      got == want) {
    rpos_ = offset;
    rlen_ = got;
  }
}

void LibReader::close() {
  rlen_ = 0;
  seek_.clear();
  if (whole_) {
    free(rbuf_);
    rbuf_ = nullptr;
    rcap_ = 0;
    whole_ = false;
  } else if (open_) {
    f_close(fil_);
  }
  open_ = false;
}

bool LibReader::readBytes(uint32_t offset, void *out, uint32_t len) {
  if (!open_) {
    return false;
  }
  if (whole_) {
    if (offset > rlen_ || len > rlen_ - offset) return false;
    memcpy(out, rbuf_ + offset, len);
    return true;
  }
  cardYieldPoint();
  if (rbuf_ && len <= rcap_) {
    if (offset < rpos_ || offset + len > rpos_ + rlen_) {
      const uint32_t size = f_size(fil_);
      if (offset + len > size) {
        return false;
      }
      // Start a little before the request: scans sometimes step back a bit
      // (a folder's song names sit just before its path in names.dat).
      const uint32_t slack = std::min(rcap_ / 8, rcap_ - len);
      const uint32_t start = offset > slack ? offset - slack : 0;
      UINT n = 0;
      const UINT want = std::min<uint32_t>(rcap_, size - start);
      rlen_ = 0;
      if (seek_.seek(fil_, start) != FR_OK || f_read(fil_, rbuf_, want, &n) != FR_OK || n != want) {
        return false;
      }
      rpos_ = start;
      rlen_ = n;
    }
    memcpy(out, rbuf_ + (offset - rpos_), len);
    return true;
  }
  if (seek_.seek(fil_, offset) != FR_OK) {
    return false;
  }
  UINT n = 0;
  return f_read(fil_, out, len, &n) == FR_OK && n == len;
}

bool LibReader::read(uint32_t index, void *out, uint32_t n) {
  if (index + n > header_.recordCount) {
    return false;
  }
  return readBytes(sizeof(LibHeader) + index * header_.recordSize, out,
                   n * header_.recordSize);
}

bool LibReader::readExtra(void *out) {
  return header_.extraSize > 0 &&
         readBytes(sizeof(LibHeader) + header_.recordCount * header_.recordSize, out,
                   header_.extraSize);
}

bool LibReader::verify() {
  if (!open_) {
    return false;
  }
  static uint8_t buf[2048];
  uint32_t left = header_.recordCount * header_.recordSize + header_.extraSize;
  uint32_t crc = 0;
  if (whole_) {
    return crc32(0, rbuf_ + sizeof(LibHeader), left) == header_.dataCrc;
  }
  if (f_lseek(fil_, sizeof(LibHeader)) != FR_OK) {
    return false;
  }
  while (left > 0) {
    const UINT want = std::min<uint32_t>(left, sizeof(buf));
    UINT n = 0;
    if (f_read(fil_, buf, want, &n) != FR_OK || n != want) {
      return false;
    }
    crc = crc32(crc, buf, n);
    left -= n;
  }
  return crc == header_.dataCrc;
}

// ================================================================ LibStore
namespace LibStore {

String genPath(uint32_t gen) {
  char path[40];
  snprintf(path, sizeof(path), LIB_DIR "/gen-%08lu", static_cast<unsigned long>(gen));
  return path;
}

String filePath(uint32_t gen, LibFileType type) {
  if (type == LF_TEMP_SKELETON) {
    return genPath(gen) + "/skeleton.tmp";
  }
  if (type == LF_TITLE_KEYS || type == LF_ARTIST_KEYS) {
    return genPath(gen) + (type == LF_TITLE_KEYS ? "/title.keys" : "/artist.keys");
  }
  return genPath(gen) + "/" + kLibFileNames[type];
}

static uint16_t recordSizeOf(LibFileType type) {
  switch (type) {
    case LF_MANIFEST: return sizeof(ManifestRecord);
    case LF_NAMES: return 1;
    case LF_FOLDERS: return sizeof(FolderRecord);
    case LF_SONGS: return sizeof(SongRecord);
    case LF_ARTISTS: return sizeof(ArtistRecord);
    case LF_ALBUMS: return sizeof(AlbumRecord);
    default: return sizeof(uint32_t);  // indexes
  }
}

// Generation numbers present on the card (valid or not), newest first.
static std::vector<uint32_t> listGenerations() {
  std::vector<uint32_t> gens;
  FF_DIR dir;
  if (f_opendir(&dir, cardPath(LIB_DIR).c_str()) != FR_OK) {
    return gens;
  }
  static FILINFO info;
  while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
    if ((info.fattrib & AM_DIR) && strncmp(info.fname, "gen-", 4) == 0) {
      gens.push_back(strtoul(info.fname + 4, nullptr, 10));
    }
  }
  f_closedir(&dir);
  std::sort(gens.begin(), gens.end(), std::greater<uint32_t>());
  return gens;
}

bool validateGeneration(uint32_t gen, ManifestRecord *out) {
  LibReader reader;
  ManifestRecord manifest;
  if (!reader.open(gen, LF_MANIFEST, sizeof(ManifestRecord)) || reader.count() != 1 ||
      !reader.verify() || !reader.read(0, &manifest)) {
    return false;
  }
  for (uint8_t t = LF_NAMES; t < LF_TYPE_COUNT; t++) {
    const LibFileType type = static_cast<LibFileType>(t);
    if (!reader.open(gen, type, recordSizeOf(type)) ||
        reader.header().dataCrc != manifest.fileCrc[t] ||
        reader.count() != manifest.fileCount[t]) {
      return false;
    }
  }
  if (out) {
    *out = manifest;
  }
  return true;
}

uint32_t findCurrent(ManifestRecord *manifest) {
  for (uint32_t gen : listGenerations()) {
    if (validateGeneration(gen, manifest)) {
      return gen;
    }
  }
  return LIB_NONE;
}

bool verifyGeneration(uint32_t gen) {
  if (!validateGeneration(gen, nullptr)) {
    return false;
  }
  LibReader reader;
  for (uint8_t t = LF_NAMES; t < LF_TYPE_COUNT; t++) {
    const LibFileType type = static_cast<LibFileType>(t);
    if (!reader.open(gen, type, recordSizeOf(type)) || !reader.verify()) {
      return false;
    }
  }
  return true;
}

uint32_t newGeneration() {
  const String root = cardPath(LIB_DIR);
  FILINFO info;
  if (f_stat(root.c_str(), &info) != FR_OK) {
    const FRESULT fr = f_mkdir(root.c_str());
    if (fr != FR_OK) {
      Serial.printf("newGeneration: mkdir %s failed (FatFs error %d)\n", LIB_DIR, fr);
      return LIB_NONE;
    }
    f_chmod(root.c_str(), AM_HID, AM_HID);  // hidden from Windows Explorer
  }
  const std::vector<uint32_t> gens = listGenerations();
  const uint32_t gen = gens.empty() ? 1 : gens.front() + 1;
  if (gen == LIB_NONE) {
    return LIB_NONE;
  }
  const FRESULT fr = f_mkdir(cardPath(genPath(gen)).c_str());
  if (fr != FR_OK) {
    Serial.printf("newGeneration: mkdir %s failed (FatFs error %d)\n", genPath(gen).c_str(), fr);
    return LIB_NONE;
  }
  return gen;
}

bool commit(uint32_t gen, ManifestRecord manifest) {
  LibReader reader;
  for (uint8_t t = LF_NAMES; t < LF_TYPE_COUNT; t++) {
    const LibFileType type = static_cast<LibFileType>(t);
    if (!reader.open(gen, type, recordSizeOf(type)) || !reader.verify()) {
      return false;  // read-back failed: never make this generation live
    }
    manifest.fileCrc[t] = reader.header().dataCrc;
    manifest.fileCount[t] = reader.count();
  }
  reader.close();
  LibWriter writer;
  return writer.create(gen, LF_MANIFEST, sizeof(ManifestRecord)) &&
         writer.append(&manifest) && writer.finish();
}

bool removeGeneration(uint32_t gen) {
  const String dirPath = cardPath(genPath(gen));
  std::vector<String> files;
  FF_DIR dir;
  if (f_opendir(&dir, dirPath.c_str()) == FR_OK) {
    static FILINFO info;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
      files.push_back(dirPath + "/" + info.fname);
    }
    f_closedir(&dir);
  }
  // The manifest goes first, so a half-deleted generation is never "valid".
  f_unlink((dirPath + "/" + kLibFileNames[LF_MANIFEST]).c_str());
  for (const String &file : files) {
    f_unlink(file.c_str());
  }
  return f_unlink(dirPath.c_str()) == FR_OK;
}

uint32_t newestGeneration() {
  const std::vector<uint32_t> gens = listGenerations();
  return gens.empty() ? LIB_NONE : gens.front();
}

void removeAllExcept(uint32_t keep, uint32_t keepToo) {
  for (uint32_t gen : listGenerations()) {
    if (gen != keep && gen != keepToo) {
      removeGeneration(gen);
    }
  }
}

}  // namespace LibStore
