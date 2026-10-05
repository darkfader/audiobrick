#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Plays /storage/<name> (.mp3 or 16-bit PCM .wav, mono or stereo, any sample rate) through a player slot
// (see media.h), once or repeatedly until stopped when loop is true. Returns false if the file is missing
// or the slot is busy. clip_play uses the main slot.
bool clip_play_slot(const char *name, bool loop, int slot);
bool clip_play(const char *name, bool loop);

// Plays a sound that is already in memory (an uploaded announcement, .mp3 or 16-bit PCM .wav) once, through a slot. Takes over the buffer
// (allocated with malloc / heap_caps_malloc) and frees it when done, or immediately if it returns false.
bool clip_play_memory(uint8_t *data, size_t len, bool is_wav, const char *label, int slot);

// A name is acceptable when it only has letters, digits, '.', '-' and '_' and ends in .mp3 or .wav.
bool clip_name_valid(const char *name);
