// Software parametric EQ in the audio path (RBJ biquads, same settings for both channels).
//
// The preamp is computed automatically: it cancels the largest boost of the combined curve, so the
// EQ never raises the output above what the speaker-profile loudness cap already assumes.
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EQ_MAX_BANDS 6

typedef enum { EQ_PEAK = 0, EQ_LOWSHELF = 1, EQ_HIGHSHELF = 2 } eq_type_t;

typedef struct {
    bool on;
    eq_type_t type;
    float f0;       // Hz, 20..20000
    float q;        // 0.2..10 (shelves: 0.7 is a gentle slope)
    float gain_db;  // -15..+6
} eq_band_t;

typedef struct {
    bool enabled;
    eq_band_t band[EQ_MAX_BANDS];
} eq_config_t;

#if CONFIG_AB_FEATURE_EQ
void eq_init(void);                       // loads from NVS, else the starter preset
eq_config_t eq_get(void);
bool eq_set(const eq_config_t *cfg);      // validates, stores, applies
void eq_reset_default(void);
float eq_preamp_db(void);                 // 0 or negative; applied only while the EQ is enabled
float eq_response_db(float freq_hz);      // combined response including the preamp (for the UI and tests)
void eq_process(int16_t *stereo, size_t frames);  // audio task only; in place
#else  // not built: the audio path simply skips the EQ
static inline void eq_init(void) {}
static inline void eq_process(int16_t *stereo, size_t frames) { (void)stereo; (void)frames; }
#endif
