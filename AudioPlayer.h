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
bool audioPlay(const char *path, uint32_t startOffset = 0);
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

// Times the output ran dry during playback (audible gaps), since boot.
uint32_t audioUnderruns();

// The decoder and the read-ahead ring (about 45 KB) can be freed while
// nothing plays, for album art downloads. Only when stopped or paused (false
// while playing). A paused song keeps its place: audioResume() rebuilds the
// engine and continues from there; audioPlay() rebuilds it too. The I2S
// output keeps running (silence), so the amp's inputs stay driven.
bool audioReleaseEngine();
bool audioEngineReleased();
// Called (from the task asking to play) before the engine is rebuilt, so
// whatever is using the memory can give it back first. It may block.
void audioSetBeforeRestore(void (*fn)());
