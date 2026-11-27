#pragma once
// On-card music library format (version 2).
//
// Layout on the card:
//   /.mp3player/gen-NNNNNNNN/   one complete library generation
//     names.dat    UTF-8 string heap: folder paths and file names (exact, for opening files)
//     folders.dat  FolderRecord per folder, id = record number (root is 0)
//     songs.dat    SongRecord per song, stored folder by folder; id = record number
//     artists.dat  ArtistRecord per artist, sorted by name (extra: JumpTable)
//     albums.dat   AlbumRecord per album, grouped by artist, then year, then name
//     title.idx    song ids sorted by title (extra: JumpTable)
//     artist.idx   song ids by artist, year, album, disc, track; each artist's
//                  and each album's songs are contiguous (no extra)
//     album.idx    album ids sorted by album name (extra: JumpTable)
//     none.idx     song ids of every artist's "none" album, by title
//     manifest.dat one ManifestRecord, written LAST
//     title.keys, artist.keys  optional: the sorted 48-byte sort records behind
//                  title.idx / artist.idx, so the next build can merge in a
//                  few changed songs instead of re-sorting (not in the manifest;
//                  checked by their own data CRC before use)
//
// Artists are grouped by album artist when set, else track artist. Sorting
// ignores case and a leading "The ". Missing artists/albums sort last as
// "Unknown Artist" / "Unknown Album".
//
// A generation is valid only if its manifest validates and every file's
// header matches the manifest. The newest valid generation is used; others
// are deleted. A crash while building leaves a generation without a manifest,
// which is ignored and cleaned up.
//
// Every file: LibHeader (32 bytes), then recordCount fixed-size records, then
// extraSize bytes of extra data. Records start at offset 32, so record i is at
// 32 + i * recordSize. Display strings are already converted to the screen's
// ASCII font (see Text.h), NUL-terminated and truncated to fit.

#include <Arduino.h>

#define LIB_DIR "/.mp3player"
#define LIB_MAGIC "MP3L"
// Song text is cleaned while tagging (LibClean.h); new rules need every song
// retagged, so their version is part of the format version.
#define LIB_CLEAN_VERSION 6  // 6: ideographic spaces, full-width ASCII; 5: undrawable tags -> file/folder names; 4: names kept as UTF-8 (accents)
// 3x: cleaned names, "none" albums, duplicates hidden (x = LIB_CLEAN_VERSION)
#define LIB_FORMAT_VERSION (30 + LIB_CLEAN_VERSION)
#define LIB_NONE 0xFFFFFFFFu

enum LibFileType : uint8_t {
  LF_MANIFEST = 1,
  LF_NAMES,
  LF_FOLDERS,
  LF_SONGS,
  LF_ARTISTS,
  LF_ALBUMS,
  LF_TITLE_INDEX,
  LF_ARTIST_INDEX,
  LF_ALBUM_INDEX,
  LF_NONE_INDEX,
  LF_TYPE_COUNT,
  // Indexer working files (never part of a valid generation).
  LF_TEMP_SKELETON = 20,
  // Optional sort caches (see title.keys above).
  LF_TITLE_KEYS = 21,
  LF_ARTIST_KEYS = 22,
};

// File name for each type, indexed by LibFileType.
extern const char *const kLibFileNames[LF_TYPE_COUNT];

struct __attribute__((packed)) LibHeader {
  char magic[4];          // "MP3L"
  uint8_t type;           // LibFileType
  uint8_t version;        // LIB_FORMAT_VERSION
  uint16_t recordSize;
  uint32_t recordCount;
  uint32_t generation;
  uint16_t extraSize;
  uint16_t reserved0;
  uint32_t dataCrc;       // CRC-32 of records followed by extra
  uint32_t reserved1;
  uint32_t headerCrc;     // CRC-32 of the 28 bytes above
};
static_assert(sizeof(LibHeader) == 32, "LibHeader size");

struct __attribute__((packed)) SongRecord {
  uint32_t folderId;
  uint32_t nameOffset;    // file name in names.dat
  uint16_t nameLen;
  uint16_t flags;         // reserved
  uint32_t fileSize;      // with mtime: change detection
  uint32_t mtime;         // FAT date << 16 | FAT time
  uint32_t durationMs;
  uint32_t audioStart;    // byte range of the MPEG audio, for elapsed time
  uint32_t audioEnd;
  uint32_t artistId;      // reserved (LIB_NONE)
  uint32_t albumId;       // reserved (LIB_NONE)
  uint16_t track;
  uint16_t year;
  uint8_t disc;
  uint8_t durationSource; // DurationSource
  uint16_t reserved0;
  char title[64];         // display text (falls back to file name)
  char artist[48];        // track artist, display text
  char album[48];
  char albumArtist[32];
  uint8_t reserved1[16];
};
static_assert(sizeof(SongRecord) == 256, "SongRecord size");

struct __attribute__((packed)) FolderRecord {
  uint32_t parentId;      // LIB_NONE for the root
  uint32_t pathOffset;    // full path in names.dat ("" for the root)
  uint16_t pathLen;
  uint16_t depth;
  uint32_t firstChild;    // child folders are contiguous ids
  uint32_t childCount;
  uint32_t firstSong;     // a folder's songs are contiguous ids
  uint32_t songCount;
  char name[36];          // display text
};
static_assert(sizeof(FolderRecord) == 64, "FolderRecord size");

struct __attribute__((packed)) ArtistRecord {
  char name[48];
  uint32_t firstAlbum;    // an artist's albums are contiguous ids
  uint32_t albumCount;
  uint32_t firstSongPos;  // position in artist.idx
  uint32_t songCount;
};
static_assert(sizeof(ArtistRecord) == 64, "ArtistRecord size");

// AlbumRecord flags. An artist's songs from albums of 3 songs or fewer and
// without an album are their "none" album (ALBUM_NONE, by title, last of
// the artist's albums, not in album.idx). The Albums tab's "none" is one
// more record (ALBUM_NONE_ALL, artistId LIB_NONE) over none.idx.
#define ALBUM_NONE 1
#define ALBUM_NONE_ALL 2
#define NONE_ALBUM_NAME "none"
#define NONE_MAX_SONGS 3  // albums with this many songs or fewer go to "none"

struct __attribute__((packed)) AlbumRecord {
  char name[44];
  uint32_t artistId;
  uint16_t year;
  uint16_t flags;         // ALBUM_NONE, ALBUM_NONE_ALL
  uint32_t firstSongPos;  // position in artist.idx (an album's songs are contiguous); none.idx for ALBUM_NONE_ALL
  uint32_t songCount;
  uint32_t reserved1;
};
static_assert(sizeof(AlbumRecord) == 64, "AlbumRecord size");

// Extra data of title.idx, album.idx and artists.dat. Bucket 0 is '#'
// (anything not A-Z), then A..Z; start[b] is the first position of bucket b,
// start[27] the total. "Unknown" entries sort after Z, inside Z's range.
struct __attribute__((packed)) JumpTable {
  uint32_t start[28];
};

// Index entries are uint32_t ids: song ids, except album.idx (album ids).
// Songs that are duplicates of another (same title and artist after
// cleaning, length within 2 s) are in no index; their folders still list them.

struct __attribute__((packed)) ManifestRecord {
  uint32_t songCount;
  uint32_t folderCount;
  uint32_t artistCount;
  uint32_t albumCount;
  uint32_t nameBytes;
  uint32_t buildMs;
  uint32_t fileCrc[LF_TYPE_COUNT];   // dataCrc of each file, by type
  uint32_t fileCount[LF_TYPE_COUNT]; // recordCount of each file, by type
  uint32_t listingSum;   // fingerprint of the card's MP3 listing: sums of two
  uint32_t listingSum2;  // hashes of each file's path, size and time (order-
                         // independent); equal => nothing to re-index
  uint32_t reserved[2];
};

// Sort bucket of a display string: 0 for '#', 1..26 for A..Z.
uint8_t libBucket(const char *text);
