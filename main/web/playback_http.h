#pragma once
// Remembers which clip or announcement was started on top of a running stream, so that Stop can end exactly that one.
void playback_overlay_set(const char *name);
