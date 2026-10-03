#include "limits.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "limits";

static speaker_profile_t s_profile;

static const speaker_profile_t DEFAULT_PROFILE = {
    .name = "Yamaha NS-B40",
    .ohms = 6.0f,
    .sens_db = 83.0f,
    .dist_m = 1.0f,
    .max_spl_db = 70.0f,  // deliberately quiet for a small room
    .rated_w = 30.0f,
    .min_hz = 100.0f,
};

static bool in_range(float v, float lo, float hi)
{
    return v >= lo && v <= hi;  // false for NaN as well
}

static bool valid(const speaker_profile_t *p)
{
    return in_range(p->ohms, 3.2f, 64.0f) && in_range(p->sens_db, 60.0f, 120.0f) &&
           in_range(p->dist_m, 0.1f, 10.0f) && in_range(p->max_spl_db, 40.0f, ABS_MAX_SPL_DB) &&
           in_range(p->rated_w, 0.5f, 200.0f) && in_range(p->min_hz, 20.0f, 2000.0f) &&
           memchr(p->name, '\0', sizeof p->name) != NULL;
}

void limits_init(void)
{
    s_profile = DEFAULT_PROFILE;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) != ESP_OK) return;
    speaker_profile_t p;
    size_t len = sizeof p;
    if (nvs_get_blob(h, "profile", &p, &len) == ESP_OK && len == sizeof p && valid(&p)) {
        s_profile = p;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "profile '%s': %.1f ohm, %.1f dB, max %.0f dB SPL at %.2f m",
             s_profile.name, s_profile.ohms, s_profile.sens_db, s_profile.max_spl_db, s_profile.dist_m);
}

speaker_profile_t limits_get(void)
{
    return s_profile;
}

bool limits_set(const speaker_profile_t *p)
{
    if (!valid(p)) return false;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "profile", p, sizeof *p) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) s_profile = *p;
    return ok;
}

float limits_max_vrms(void)
{
    const speaker_profile_t *p = &s_profile;
    float v_spl = 2.83f * powf(10.0f, (p->max_spl_db - p->sens_db + 20.0f * log10f(p->dist_m)) / 20.0f);
    float v_pow = sqrtf(0.5f * p->rated_w * p->ohms);  // half the rating: a steady sine is a hard load
    return fminf(v_spl, v_pow);
}

int limits_max_volume_db(void)
{
    float vmax_pk = limits_max_vrms() * 1.41421356f;
    int db = (int)floorf(20.0f * log10f(vmax_pk / FULLSCALE_VPK));
    if (db > 0) db = 0;
    if (db < -90) db = -90;
    return db;
}

void limits_estimate(float tone_dbfs, float vol_db, float pvdd, float *vrms, float *watts, float *spl)
{
    const speaker_profile_t *p = &s_profile;
    float vpk = FULLSCALE_VPK * powf(10.0f, (tone_dbfs + vol_db) / 20.0f);
    if (pvdd > 0.0f && vpk > 0.9f * pvdd) vpk = 0.9f * pvdd;  // the supply clips the output
    float v = vpk / 1.41421356f;
    *vrms = v;
    *watts = v * v / p->ohms;
    *spl = v > 1e-6f ? p->sens_db + 20.0f * log10f(v / 2.83f) - 20.0f * log10f(p->dist_m) : 0.0f;
}
