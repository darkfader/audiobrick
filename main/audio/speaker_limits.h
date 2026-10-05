// Speaker profile and the loudness limit derived from it.
//
// Model: SPL(d) = sensitivity + 20*log10(Vrms / 2.83 V) - 20*log10(d / 1 m).
// The amp's full-scale output swing (analog gain 0 dB) is ~29.5 V peak per the datasheet; the
// real peak is also bounded by the supply (PVDD). These are estimates, not calibrated readings.
#pragma once
#include <stdbool.h>

#define FULLSCALE_VPK   29.5f   // TAS5825M analog gain 0 dB, peak volts at 0 dBFS (datasheet)
#define ABS_MAX_SPL_DB  100.0f  // no profile may allow more than this

typedef struct {
    char  name[24];
    float ohms;        // nominal impedance
    float sens_db;     // dB SPL at 2.83 V, 1 m
    float dist_m;      // listening distance
    float max_spl_db;  // user limit at the listening distance
    float rated_w;     // nominal (continuous) power rating
    float min_hz;      // lowest tone frequency the speaker should get
} speaker_profile_t;

void limits_init(void);                        // load from NVS, else defaults (quiet)
speaker_profile_t limits_get(void);
bool limits_set(const speaker_profile_t *p);   // validates, stores in NVS
float limits_max_vrms(void);                   // min of the SPL limit and half the rated power
int   limits_max_volume_db(void);              // amp volume cap so even 0 dBFS respects the limit

// Estimate for a sine at tone_dbfs with the amp digital volume vol_db and supply pvdd (volts).
void limits_estimate(float tone_dbfs, float vol_db, float pvdd, float *vrms, float *watts, float *spl);
