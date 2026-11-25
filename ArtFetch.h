#pragma once
// Album art downloads (MusicBrainz search, Cover Art Archive 250 px front
// cover) and artist photos (Deezer search, 250 px) into
// LIB_DIR "/art/xx/<key>.jpg"; albums and artists without one get a
// "<key>.no" marker, retried after 30 days. <key> is a hash of album artist
// and album name, so the cache survives library rebuilds.
//
// Runs in a background task on core 0, only while no music plays: WiFi and
// TLS need about 90 KB, which the player only has with the audio engine
// released (audioReleaseEngine). Pressing play stops a download within a few
// seconds and rebuilds the engine (audioSetBeforeRestore hook).
#include <Arduino.h>

void artFetchInit();                  // once, in setup()
void artFetchSetEnabled(bool on);     // per-card setting (CardSettings)
// Call every loop while the player screen is up: starts a pass when the
// player has been idle for a while and the library isn't being updated.
void artFetchTick();
void artFetchNow();                   // start a pass without waiting (Serial "artfetch")
// Test (Serial "artartists"): a pass that starts at the artist steps; the
// albums skipped are looked at by the next pass.
void artFetchArtistsNow();
void artFetchStop();                  // blocks until WiFi is off again
bool artFetchRunning();
// Progress for the status corner ("Art\n12%"), "" when not running.
String artFetchStatus();
// Thumbnail keys (ThumbStore) of an album's cover and an artist's photo; 0
// for none (no album name, "Unknown Artist").
uint32_t artAlbumKey(const char *albumArtist, const char *album);
uint32_t artArtistKey(const char *artist);
// Cache path of an album's cover ("" if the name is empty).
String artCoverPath(const char *albumArtist, const char *album);
