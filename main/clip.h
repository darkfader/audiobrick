#pragma once
#include <stdbool.h>

// Plays /storage/<name> (.mp3 or 16-bit PCM .wav, mono or stereo, any sample rate) through a player slot
// (see media.h), once or repeatedly until stopped when loop is true. Returns false if the file is missing
// or the slot is busy. clip_play uses the main slot.
bool clip_play_slot(const char *name, bool loop, int slot);
bool clip_play(const char *name, bool loop);

// A name is acceptable when it only has letters, digits, '.', '-' and '_' and ends in .mp3 or .wav.
bool clip_name_valid(const char *name);
