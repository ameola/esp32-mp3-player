#pragma once
// Cleans a song's display text while the library is built (the files on the
// card are left as they are). Rules found in a real 8,000-song card:
//
// - URL encoding ("%20Fools%20gold") and HTML entities ("&quot;") decoded;
//   underscores as spaces, ".mp3" and repeated spaces dropped.
// - Track numbers taken off the front of titles ("01 - ", "(02) - ",
//   "04.Name", "01 Name", "1-01 "), and used as the track number when the
//   tag has none. "22 Acacia Avenue" and "2-4-6-8 Motorway" keep theirs.
// - "Track 1" titles replaced by the file's name.
// - An untagged "Artist - Title" (also "Artist - Album - 03 - Title") split
//   into its parts; "Artist - " before and " - Artist" / " - Various
//   Artists" after a title dropped when they repeat the song's artist.
// - Placeholder tags ("Artist", "Album", "title", "<Unknown>", web
//   addresses) treated as missing.
// - "Disc 2", "(CD 1)", "[CD2]" taken off album names; the number becomes
//   the song's disc, so the discs of an album group as one.
#include <Arduino.h>

#include "LibFormat.h"

// Bump LIB_CLEAN_VERSION (LibFormat.h) when the rules change: the next
// build then retags every song.

void libCleanSong(SongRecord &rec, const char *fileName);

// A name reduced for grouping: lower case letters and digits only, "&" and
// "+" as "and", without a leading "the" if dropThe. Names with none of
// those (all "?") are kept lower-cased so they don't all fall together.
String libNormName(const char *name, bool dropThe);
uint32_t libNormHash(const char *name, bool dropThe);  // FNV-1a of libNormName
