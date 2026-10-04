// Shared player with three independent channels ("slots") that the I2S task mixes:
//   SLOT_MAIN  : a network stream or a clip started by the user
//   SLOT_BG    : the ambient scene's looping background
//   SLOT_EVENT : the ambient scene's one-shot events (a meow, a door ...)
// Each slot has its own buffer in PSRAM. Format inside the buffers is always 48 kHz, 16-bit, interleaved stereo.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { MEDIA_NONE = 0, MEDIA_STREAM, MEDIA_CLIP } media_kind_t;

#define MEDIA_SLOTS 3
enum { SLOT_MAIN = 0, SLOT_BG = 1, SLOT_EVENT = 2 };

bool media_init(void);
// How much audio is collected before a stream or clip starts playing, and so roughly the delay of a live stream
// (20-600 ms, default 170). Lower = less delay but more risk of dropouts when the network hiccups.
void media_set_prebuffer_ms(uint32_t ms);
uint32_t media_prebuffer_ms(void);

// Producer side. media_begin_slot fails if the slot is busy or the test tone is on.
bool media_begin_slot(int slot, media_kind_t kind, const char *label);
// Blocks while the buffer is full. Returns the frames written (less than asked only after an abort).
size_t media_write_slot(int slot, const int16_t *stereo, size_t frames);
// Never blocks: writes what fits and returns the frames written (live UDP sources must not stall).
size_t media_write_nb_slot(int slot, const int16_t *stereo, size_t frames);
void media_finish_slot(int slot);   // producer has no more data; playback ends once the buffer drains
void media_abort_slot(int slot);    // stop now (works from any task)
bool media_aborted_slot(int slot);

// Consumer side (I2S task).
bool media_active_slot(int slot);
size_t media_read_slot(int slot, int16_t *out, size_t frames);  // frames available now (0 while pre-buffering)

media_kind_t media_kind_slot(int slot);
const char *media_label_slot(int slot);
uint32_t media_buffer_ms_slot(int slot);
uint32_t media_underruns_slot(int slot);

// Pause/resume of the main channel (fades out, keeps its place), and a per-channel level in dB (0 to -40).
void media_set_paused(bool paused);
bool media_paused(void);
void media_set_slot_gain_db(int slot, float db);

// The main slot is what the user's stream and clips use; these keep the older single-player API.
static inline bool media_begin(media_kind_t k, const char *l) { return media_begin_slot(SLOT_MAIN, k, l); }
static inline size_t media_write(const int16_t *s, size_t n) { return media_write_slot(SLOT_MAIN, s, n); }
static inline void media_finish(void) { media_finish_slot(SLOT_MAIN); }
static inline void media_abort(void) { media_abort_slot(SLOT_MAIN); }
static inline bool media_aborted(void) { return media_aborted_slot(SLOT_MAIN); }
static inline bool media_active(void) { return media_active_slot(SLOT_MAIN); }
static inline media_kind_t media_kind(void) { return media_kind_slot(SLOT_MAIN); }
static inline const char *media_label(void) { return media_label_slot(SLOT_MAIN); }
static inline uint32_t media_buffer_ms(void) { return media_buffer_ms_slot(SLOT_MAIN); }
static inline uint32_t media_underruns(void) { return media_underruns_slot(SLOT_MAIN); }

// Linear-interpolating converter from any input rate to 48 kHz stereo.
typedef struct {
    float step;      // input samples per output sample
    float pos;
    int16_t prev[2];
    bool primed;
} resampler_t;

void resampler_init(resampler_t *r, uint32_t in_rate);
// Converts nin stereo frames; writes at most out_cap frames and returns how many were produced.
// Choose out_cap >= nin * 48000 / in_rate + 2.
size_t resampler_process(resampler_t *r, const int16_t *in, size_t nin, int16_t *out, size_t out_cap);
