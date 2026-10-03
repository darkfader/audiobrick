// Shared player: one producer (network stream or stored clip) fills a buffer in PSRAM and the I2S
// task drains it. Format inside the buffer is always 48 kHz, 16-bit, interleaved stereo.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { MEDIA_NONE = 0, MEDIA_STREAM, MEDIA_CLIP } media_kind_t;

bool media_init(void);

// Producer side. media_begin fails if something else is playing or the test tone is on.
bool media_begin(media_kind_t kind, const char *label);
// Blocks while the buffer is full. Returns the frames written (less than asked only after an abort).
size_t media_write(const int16_t *stereo, size_t frames);
void media_finish(void);   // producer has no more data; playback ends once the buffer drains
void media_abort(void);    // stop now (works from any task)
bool media_aborted(void);

// Consumer side (I2S task).
bool media_active(void);
size_t media_read(int16_t *out, size_t frames);  // returns frames available now (0 while pre-buffering)

media_kind_t media_kind(void);
const char *media_label(void);
uint32_t media_buffer_ms(void);
uint32_t media_underruns(void);

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
