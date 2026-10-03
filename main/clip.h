#pragma once
#include <stdbool.h>

// Plays /storage/<name> (once, or repeatedly until stopped when loop is true) (.mp3 or 16-bit PCM .wav, mono or stereo, any sample rate) through the
// shared player. Returns false if the file is missing or something else is playing.
bool clip_play(const char *name, bool loop);

// A name is acceptable when it only has letters, digits, '.', '-' and '_' and ends in .mp3 or .wav.
bool clip_name_valid(const char *name);
