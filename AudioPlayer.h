#pragma once
// MP3 playback: ESP8266Audio decoder -> MAX98357A over I2S.

#include <Arduino.h>

enum PlayerState { STOPPED, PLAYING, PAUSED };

// Starts the I2S clocks immediately (silence) so the amp's inputs never
// float, and the decoder task (core 0). Call first thing in setup().
// The other functions are safe to call from any task.
void audioInit();

// Stops any current song first. startOffset: byte offset of the first MPEG
// frame (skips the ID3 tag, whose pictures can contain false frame syncs).
// False if it can't: audioPlayError() then says why when it's the file
// ("a WMA file, not an MP3"; "" for other failures).
bool audioPlay(const char *path, uint32_t startOffset = 0);
const char *audioPlayError();
void audioStop();
void audioPause();
void audioResume();

PlayerState audioState();
// True once after a song reaches its end (the player is then STOPPED).
bool audioTakeTrackEnded();
uint32_t audioFilePos();
uint32_t audioFileSize();

void audioSetVolume(int level);  // 0..VOLUME_STEPS
int audioVolume();
// Tone (Settings > Audio): bass and treble shelves in dB, 0 for flat.
void audioSetTone(int8_t bassDb, int8_t trebleDb);
// The loudest the volume control goes, in percent of full (Settings > Audio).
void audioSetVolumeLimit(uint8_t percent);

// Times the output ran dry during playback (audible gaps), since boot.
uint32_t audioUnderruns();

// The read-ahead ring is full (or holds the rest of the song), or nothing
// plays. Card work that can hold the card for long (name lookups in big
// folders) waits for this after a song starts, so the song can't run dry.
bool audioReadAheadSettled();

// The decoder and the read-ahead ring (about 45 KB of internal RAM without
// PSRAM; with it the ring is in PSRAM) can be freed while
// nothing plays, for album art downloads. Only when stopped or paused (false
// while playing). A paused song keeps its place: audioResume() rebuilds the
// engine and continues from there; audioPlay() rebuilds it too. The I2S
// output keeps running (silence), so the amp's inputs stay driven.
bool audioReleaseEngine();
bool audioEngineReleased();
// Called (from the task asking to play) before the engine is rebuilt, so
// whatever is using the memory can give it back first. It may block.
void audioSetBeforeRestore(void (*fn)());
