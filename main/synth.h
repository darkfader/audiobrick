// Polyphonic synthesizer controlled over OSC (UDP port 9000).
//
// OSC addresses (floats and ints are accepted for every number):
//   /synth/note   note vel          MIDI note 0-127 and velocity 1-127 (velocity 0 = note off)
//   /synth/noteoff note
//   /synth/alloff
//   /synth/freq   hz [vel]          monophonic voice at a frequency (vel 0 = off, default 100)
//   /synth/gate   0|1               gate the monophonic voice
//   /synth/wave   0-3 | sine|tri|saw|square
//   /synth/adsr   a d s r           seconds, seconds, level 0-1, seconds
//   /synth/cutoff hz                master low-pass, 50-20000
//   /synth/volume 0-1
// The synth is capped like every other source: its output never exceeds full scale, so the speaker profile's
// loudness limit (set through the amp volume) always holds. Notes below the profile's lowest frequency are ignored.
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#define OSC_PORT 9000

typedef struct {
    bool enabled;
    char allow_ip[16];     // "" accepts OSC from any host
    uint8_t wave;          // 0 sine, 1 triangle, 2 saw, 3 square
    float a, d, s, r;      // envelope: attack, decay (seconds), sustain level (0-1), release (seconds)
    float cutoff;          // master low-pass in Hz
    float volume;          // 0..1
    float max_note_s;      // a note that is never released stops after this long (1..600 s)
} synth_cfg_t;

#if CONFIG_AB_FEATURE_SYNTH
void synth_init(void);
synth_cfg_t synth_get(void);
bool synth_set(const synth_cfg_t *cfg);

// Called from the audio task for each block: writes frames of left/right samples (full scale = +-32767) and
// returns true if anything is sounding. Never blocks.
bool synth_render(float *left, float *right, int frames);
bool synth_busy(void);                 // voices sounding or notes queued

void synth_note(int note, int velocity, uint32_t dur_ms);   // from any task; dur_ms 0 = until released
void synth_http_register(httpd_handle_t server);
#else  // not built
static inline void synth_init(void) {}
static inline bool synth_render(float *l, float *r, int n) { (void)l; (void)r; (void)n; return false; }
static inline bool synth_busy(void) { return false; }
#endif
