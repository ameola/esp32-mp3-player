#pragma once
// Read access to the live on-card library for the UI. Records are read from
// the card a few at a time; nothing is held in RAM. Files beyond the core
// four are opened on first use (each open file costs ~4 KB of heap).

#include <Arduino.h>

#include "LibFormat.h"

struct Track {
  uint32_t id = 0;          // song id in the open generation
  String title;             // display text (file name if untagged)
  String artist;            // empty when missing
  String album;
  String albumArtist;       // empty when missing
  uint16_t track = 0;
  uint8_t disc = 0;
  uint16_t year = 0;
  uint32_t durationMs = 0;
  uint32_t audioStart = 0;  // byte range of the MPEG audio, for elapsed time
  uint32_t audioEnd = 0;
  uint32_t fileSize = 0;
  uint32_t startCluster = 0;  // opens the file without a name lookup (0: by path)
};

enum JumpKind { JUMP_TITLES, JUMP_ARTISTS, JUMP_ALBUMS };

namespace Catalog {

bool open(uint32_t gen);  // opens the generation; closes any previous one
void close();
bool isOpen();
uint32_t generation();

uint32_t songCount();   // every song file (ids 0..songCount-1)
uint32_t titleCount();  // songs in the lists (title.idx): duplicates left out
bool song(uint32_t id, Track &out);
String songPath(uint32_t id);  // Arduino-style path on the card

// Song ids at positions of the sorted indexes.
bool titleSong(uint32_t pos, uint32_t &id);   // title.idx: all songs by title
bool artistSong(uint32_t pos, uint32_t &id);  // artist.idx: by artist/album/track
bool noneSong(uint32_t pos, uint32_t &id);    // none.idx: every "none" album's songs by title

uint32_t artistCount();
bool artist(uint32_t id, ArtistRecord &out);
uint32_t albumCount();       // every album record, artists' "none" albums included
uint32_t albumIndexCount();  // albums in the Albums tab (album.idx)
bool album(uint32_t id, AlbumRecord &out);
bool albumByName(uint32_t pos, uint32_t &id);  // album.idx: albums by name

uint32_t folderCount();
bool folder(uint32_t id, FolderRecord &out);

// A-Z jump table of a sorted list (nullptr if unavailable).
const JumpTable *jump(JumpKind kind);

// Position in title order of the song with this path (title speeds the
// search). False if it's no longer in the library.
bool findTitlePos(const String &path, const String &title, uint32_t &pos);

}  // namespace Catalog
