#include "eq.h"

#if CONFIG_AB_FEATURE_EQ


#include <math.h>
#include <string.h>
#include "board.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "eq";

typedef struct { float b0, b1, b2, a1, a2; } biquad_t;

typedef struct {
    bool enabled;
    int n;
    biquad_t bq[EQ_MAX_BANDS];
    float preamp;  // linear
} coefs_t;

static coefs_t s_coefs[2];             // double buffer: the audio task reads the active one
static volatile int s_active;
static eq_config_t s_cfg;
static float s_preamp_db;
static float s_z1[2][EQ_MAX_BANDS], s_z2[2][EQ_MAX_BANDS];  // filter state, audio task only

static const eq_config_t STARTER = {
    .enabled = true,
    .band = {
        { true, EQ_PEAK, 250.0f, 0.8f, -3.0f },        // small bump near 250 Hz
        { true, EQ_HIGHSHELF, 6000.0f, 0.7f, 3.0f },   // gentle treble lift above 6 kHz
        { false, EQ_PEAK, 1000.0f, 1.0f, 0.0f },
        { false, EQ_PEAK, 2200.0f, 1.0f, 0.0f },
        { false, EQ_PEAK, 4000.0f, 1.0f, 0.0f },
        { false, EQ_PEAK, 8000.0f, 1.0f, 0.0f },
    },
};

static biquad_t design(const eq_band_t *b)
{
    float A = powf(10.0f, b->gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * b->f0 / SAMPLE_RATE;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * b->q);
    float b0, b1, b2, a0, a1, a2;
    switch (b->type) {
    case EQ_LOWSHELF: {
        float s = 2.0f * sqrtf(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - s;
        break;
    }
    case EQ_HIGHSHELF: {
        float s = 2.0f * sqrtf(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - s;
        break;
    }
    default:  // EQ_PEAK
        b0 = 1 + alpha * A;
        b1 = -2 * cw;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cw;
        a2 = 1 - alpha / A;
        break;
    }
    biquad_t q = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    return q;
}

static float biquad_db(const biquad_t *q, float freq)
{
    float w = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
    float c1 = cosf(w), s1 = sinf(w), c2 = cosf(2 * w), s2 = sinf(2 * w);
    float nr = q->b0 + q->b1 * c1 + q->b2 * c2, ni = -(q->b1 * s1 + q->b2 * s2);
    float dr = 1 + q->a1 * c1 + q->a2 * c2, di = -(q->a1 * s1 + q->a2 * s2);
    float num = nr * nr + ni * ni, den = dr * dr + di * di;
    return 10.0f * log10f((num + 1e-20f) / (den + 1e-20f));
}

static float raw_response_db(const coefs_t *c, float freq)
{
    float sum = 0;
    for (int i = 0; i < c->n; i++) sum += biquad_db(&c->bq[i], freq);
    return sum;
}

static bool valid(const eq_config_t *c)
{
    for (int i = 0; i < EQ_MAX_BANDS; i++) {
        const eq_band_t *b = &c->band[i];
        if (!(b->type >= EQ_PEAK && b->type <= EQ_HIGHSHELF)) return false;
        if (!(b->f0 >= 20.0f && b->f0 <= 20000.0f)) return false;
        if (!(b->q >= 0.2f && b->q <= 10.0f)) return false;
        if (!(b->gain_db >= -15.0f && b->gain_db <= 6.0f)) return false;
    }
    return true;
}

static void apply(const eq_config_t *cfg)
{
    coefs_t *c = &s_coefs[1 - s_active];
    memset(c, 0, sizeof *c);
    c->enabled = cfg->enabled;
    for (int i = 0; i < EQ_MAX_BANDS; i++) {
        if (cfg->band[i].on) c->bq[c->n++] = design(&cfg->band[i]);
    }
    float peak = 0.0f;  // largest boost across the audible range
    for (int i = 0; i < 200; i++) {
        float f = 20.0f * powf(1000.0f, i / 199.0f);
        float r = raw_response_db(c, f);
        if (r > peak) peak = r;
    }
    s_preamp_db = -peak;
    c->preamp = powf(10.0f, s_preamp_db / 20.0f);
    s_cfg = *cfg;
    s_active = 1 - s_active;  // publish
}

void eq_init(void)
{
    eq_config_t cfg = STARTER;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        eq_config_t stored;
        size_t len = sizeof stored;
        if (nvs_get_blob(h, "eq", &stored, &len) == ESP_OK && len == sizeof stored && valid(&stored)) cfg = stored;
        nvs_close(h);
    }
    apply(&cfg);
    ESP_LOGI(TAG, "EQ %s, preamp %.1f dB", cfg.enabled ? "on" : "bypassed", s_preamp_db);
}

eq_config_t eq_get(void) { return s_cfg; }
float eq_preamp_db(void) { return s_preamp_db; }

bool eq_set(const eq_config_t *cfg)
{
    if (!valid(cfg)) return false;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "eq", cfg, sizeof *cfg) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) apply(cfg);
    return ok;
}

void eq_reset_default(void)
{
    eq_set(&STARTER);
}

float eq_response_db(float freq_hz)
{
    const coefs_t *c = &s_coefs[s_active];
    if (!c->enabled) return 0.0f;
    return raw_response_db(c, freq_hz) + s_preamp_db;
}

void eq_process(int16_t *stereo, size_t frames)
{
    const coefs_t *c = &s_coefs[s_active];
    if (!c->enabled) return;
    for (size_t i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = stereo[2 * i + ch] * c->preamp;
            for (int b = 0; b < c->n; b++) {
                const biquad_t *q = &c->bq[b];
                float y = q->b0 * x + s_z1[ch][b];
                s_z1[ch][b] = q->b1 * x - q->a1 * y + s_z2[ch][b];
                s_z2[ch][b] = q->b2 * x - q->a2 * y;
                x = y;
            }
            if (x > 32767.0f) x = 32767.0f;
            if (x < -32768.0f) x = -32768.0f;
            stereo[2 * i + ch] = (int16_t)x;
        }
    }
}

#endif  // CONFIG_AB_FEATURE_EQ
