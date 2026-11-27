#include "Catalog.h"

#include "LibStore.h"

namespace Catalog {

static LibReader songs, titles, folders, names;          // always open
static LibReader artistIdx, artists, albums, albumIdx, noneIdx;  // opened on first use
static uint32_t gen_ = LIB_NONE;
static JumpTable jumps[3];
static bool jumpLoaded[3];

bool open(uint32_t gen) {
  close();
  if (!songs.open(gen, LF_SONGS, sizeof(SongRecord)) ||
      !titles.open(gen, LF_TITLE_INDEX, sizeof(uint32_t)) ||
      !folders.open(gen, LF_FOLDERS, sizeof(FolderRecord)) || !names.open(gen, LF_NAMES, 1)) {
    close();
    return false;
  }
  gen_ = gen;
  return true;
}

void close() {
  songs.close();
  titles.close();
  folders.close();
  names.close();
  artistIdx.close();
  artists.close();
  albums.close();
  albumIdx.close();
  noneIdx.close();
  gen_ = LIB_NONE;
  jumpLoaded[0] = jumpLoaded[1] = jumpLoaded[2] = false;
}

bool isOpen() {
  return gen_ != LIB_NONE;
}

uint32_t generation() {
  return gen_;
}

// Opens a lazily-loaded file of the current generation if needed.
static bool ensure(LibReader &reader, LibFileType type, uint16_t recordSize) {
  return reader.isOpen() || (isOpen() && reader.open(gen_, type, recordSize));
}

// ---------------------------------------------------------------- Songs
uint32_t songCount() {
  return isOpen() ? songs.count() : 0;
}

bool song(uint32_t id, Track &out) {
  SongRecord r;
  if (!isOpen() || !songs.read(id, &r)) {
    return false;
  }
  out.id = id;
  out.title = r.title;
  out.artist = r.artist;
  out.album = r.album;
  out.albumArtist = r.albumArtist;
  out.track = r.track;
  out.disc = r.disc;
  out.year = r.year;
  out.durationMs = r.durationMs;
  out.audioStart = r.audioStart;
  out.audioEnd = r.audioEnd;
  out.fileSize = r.fileSize;
  out.startCluster = r.startCluster;
  return true;
}

static String readName(uint32_t offset, uint16_t len) {
  static char buf[1025];
  len = min<uint16_t>(len, 1024);
  if (len == 0 || !names.readBytes(sizeof(LibHeader) + offset, buf, len)) {
    return "";
  }
  buf[len] = 0;
  return String(buf);
}

String songPath(uint32_t id) {
  SongRecord r;
  FolderRecord f;
  if (!isOpen() || !songs.read(id, &r) || !folders.read(r.folderId, &f)) {
    return "";
  }
  return readName(f.pathOffset, f.pathLen) + "/" + readName(r.nameOffset, r.nameLen);
}

uint32_t titleCount() {
  return isOpen() ? titles.count() : 0;
}

bool titleSong(uint32_t pos, uint32_t &id) {
  return isOpen() && titles.read(pos, &id);
}

bool artistSong(uint32_t pos, uint32_t &id) {
  return ensure(artistIdx, LF_ARTIST_INDEX, sizeof(uint32_t)) && artistIdx.read(pos, &id);
}

bool noneSong(uint32_t pos, uint32_t &id) {
  return ensure(noneIdx, LF_NONE_INDEX, sizeof(uint32_t)) && noneIdx.read(pos, &id);
}

// ---------------------------------------------------------------- Artists, albums, folders
uint32_t artistCount() {
  return ensure(artists, LF_ARTISTS, sizeof(ArtistRecord)) ? artists.count() : 0;
}

bool artist(uint32_t id, ArtistRecord &out) {
  return ensure(artists, LF_ARTISTS, sizeof(ArtistRecord)) && artists.read(id, &out);
}

uint32_t albumCount() {
  return ensure(albums, LF_ALBUMS, sizeof(AlbumRecord)) ? albums.count() : 0;
}

bool album(uint32_t id, AlbumRecord &out) {
  return ensure(albums, LF_ALBUMS, sizeof(AlbumRecord)) && albums.read(id, &out);
}

uint32_t albumIndexCount() {
  return ensure(albumIdx, LF_ALBUM_INDEX, sizeof(uint32_t)) ? albumIdx.count() : 0;
}

bool albumByName(uint32_t pos, uint32_t &id) {
  return ensure(albumIdx, LF_ALBUM_INDEX, sizeof(uint32_t)) && albumIdx.read(pos, &id);
}

uint32_t folderCount() {
  return isOpen() ? folders.count() : 0;
}

bool folder(uint32_t id, FolderRecord &out) {
  return isOpen() && folders.read(id, &out);
}

const JumpTable *jump(JumpKind kind) {
  if (!jumpLoaded[kind]) {
    bool ok = false;
    switch (kind) {
      case JUMP_TITLES: ok = isOpen() && titles.readExtra(&jumps[kind]); break;
      case JUMP_ARTISTS:
        ok = ensure(artists, LF_ARTISTS, sizeof(ArtistRecord)) && artists.readExtra(&jumps[kind]);
        break;
      case JUMP_ALBUMS:
        ok = ensure(albumIdx, LF_ALBUM_INDEX, sizeof(uint32_t)) && albumIdx.readExtra(&jumps[kind]);
        break;
    }
    if (!ok) {
      return nullptr;
    }
    jumpLoaded[kind] = true;
  }
  return &jumps[kind];
}

// ---------------------------------------------------------------- Lookup
// Same ordering the indexer sorted title.idx by: bucket, lowercase title (39).
static void titleKey(const char *title, uint8_t key[40]) {
  memset(key, 0, 40);
  key[0] = libBucket(title);
  libSortCopy(key + 1, 39, title);
}

bool findTitlePos(const String &path, const String &title, uint32_t &pos) {
  if (!isOpen()) {
    return false;
  }
  uint8_t want[40], have[40];
  titleKey(title.c_str(), want);
  uint32_t lo = 0, hi = titles.count();
  SongRecord r;
  uint32_t id;
  while (lo < hi) {  // lower bound of the title
    const uint32_t mid = (lo + hi) / 2;
    titles.read(mid, &id);
    songs.read(id, &r);
    titleKey(r.title, have);
    if (memcmp(have, want, sizeof(have)) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  for (uint32_t p = lo; p < titles.count(); p++) {  // same title: match the path
    titles.read(p, &id);
    songs.read(id, &r);
    titleKey(r.title, have);
    if (memcmp(have, want, sizeof(have)) != 0) {
      break;
    }
    if (songPath(id) == path) {
      pos = p;
      return true;
    }
  }
  return false;
}

}  // namespace Catalog
