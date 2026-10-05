#pragma once
#include <stdbool.h>

#define TONE_MIN_HZ      20.0f
#define TONE_MAX_HZ      20000.0f  // below Nyquist (24 kHz at 48 kHz sampling)
#define TONE_MAX_DBFS    (0.0f)    // digital cap; the speaker profile caps the amp volume below it
#define TONE_DEFAULT_TTL_S 15      // tone stops by itself unless it is refreshed within this time
#define TONE_MIN_DBFS    (-80.0f)

typedef struct {
    bool enabled;
    float freq_hz;
    float level_dbfs;
} tone_state_t;

// Starts I2S (BCK/WS/DOUT) and the audio task. Clocks run continuously; the output is
// silence until the tone is enabled. Parameters are clamped to the limits above.
bool tone_init(void);
void tone_set(bool enabled, float freq_hz, float level_dbfs);
tone_state_t tone_get(void);
// Dead-man timer: the tone is switched off ttl_s seconds from now unless this is called again.
void tone_hold(int ttl_s);
// While a clip or announcement plays over the main channel (a stream, radio, another clip), the main channel is lowered by this many dB
// (0 = no ducking, max 30). The change is smooth (the mixer's 100 ms ramps).
void tone_set_duck_db(int db);
int tone_duck_db(void);
