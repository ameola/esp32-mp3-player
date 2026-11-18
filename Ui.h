#pragma once
// Player screen: tabs (Songs / Artists / Albums / Folders) with drill-down
// navigation, A-Z letter strip, now-playing panel and transport controls.

#include <Arduino.h>

#include "Touch.h"

void uiShowMessage(const char *line1, const char *line2);
// Redraws only the message lines (cheap enough for progress updates).
void uiShowStatus(const char *line1, const char *line2);
void uiShowPlayer();
void uiHandleTouch(TouchPhase phase, int16_t x, int16_t y);

// Full-screen library build progress (first build, or a full rebuild).
// `redraw` draws the whole screen; otherwise only the changing parts.
// fraction: 0..1 overall progress.
void uiShowProgress(bool redraw, const char *title, const char *phase, float fraction,
                    const String &counts, const char *folder, const char *hint);

// True once after the library status corner (top right) was held for 2 s:
// the user asked for a full rebuild.
bool uiTakeRebuildRequest();

// Library status in the tab bar's right corner: up to two short lines
// separated by '\n' (e.g. "Tags\n45%"); "" to clear.
void uiSetLibraryStatus(const String &text);

// The Catalog switched to a new library generation (ids changed): re-find the
// playing song, reset navigation and redraw.
void uiLibraryChanged();

// Call every loop: handles end of song, scrolling text and the progress bar.
void uiTick();
