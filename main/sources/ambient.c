#include "ambient.h"

#if CONFIG_AB_FEATURE_AMBIENT


#include <string.h>
#include "clip.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "nvs.h"
#include "safemode.h"
#include "tone.h"

static const char *TAG = "ambient";

static ambient_config_t s_cfg;
static int64_t s_next_us[AMBIENT_MAX_RULES];  // when each event rule plays next (0 = not scheduled)
static volatile bool s_dirty = true;          // config changed: reschedule
static volatile bool s_running;
static char s_bg_clip[32];                    // background clip currently running

static const ambient_config_t DEFAULT_CFG = {
    .enabled = false,  // never starts by itself until you switch it on
    .rule = {
        { true, 1, "rain_window_loop.mp3", 0, 0, -6.0f },
        { true, 0, "cat_meow_2.mp3", 300, 900, 0.0f },
        { true, 0, "keyboard_typing_1.mp3", 600, 1800, -6.0f },
        { true, 0, "door_close_1.mp3", 900, 2400, -3.0f },
    },
};

static bool valid(const ambient_config_t *c)
{
    for (int i = 0; i < AMBIENT_MAX_RULES; i++) {
        const ambient_rule_t *r = &c->rule[i];
        if (!r->on) continue;
        if (r->type > 1) return false;
        if (memchr(r->clip, '\0', sizeof r->clip) == NULL || !clip_name_valid(r->clip)) return false;
        if (r->type == 0 && !(r->min_s >= 5 && r->max_s >= r->min_s && r->max_s <= 86400)) return false;
        if (!(r->gain_db >= -30.0f && r->gain_db <= 0.0f)) return false;
    }
    return true;
}

ambient_config_t ambient_get(void) { return s_cfg; }
bool ambient_running(void) { return s_running; }

bool ambient_set(const ambient_config_t *cfg)
{
    if (!valid(cfg)) return false;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "ambient", cfg, sizeof *cfg) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        s_cfg = *cfg;
        s_dirty = true;
    }
    return ok;
}

int ambient_next_in_s(int i)
{
    if (i < 0 || i >= AMBIENT_MAX_RULES || s_next_us[i] == 0) return -1;
    int64_t d = (s_next_us[i] - esp_timer_get_time()) / 1000000;
    return d < 0 ? 0 : (int)d;
}

static int64_t random_wait_us(const ambient_rule_t *r)
{
    uint32_t span = r->max_s - r->min_s;
    uint32_t s = r->min_s + (span ? esp_random() % (span + 1) : 0);
    return (int64_t)s * 1000000;
}

static void stop_all(void)
{
    if (media_active_slot(SLOT_BG)) media_abort_slot(SLOT_BG);
    if (media_active_slot(SLOT_EVENT)) media_abort_slot(SLOT_EVENT);
    s_bg_clip[0] = '\0';
    s_running = false;
}

static void ambient_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ambient_config_t cfg = s_cfg;
        bool blocked = media_active_slot(SLOT_MAIN) || tone_get().enabled;  // the main channel always wins
        if (!cfg.enabled || blocked) {
            if (s_running) {
                stop_all();
                s_dirty = true;  // reschedule from scratch when it may run again
            }
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (s_dirty) {
            s_dirty = false;
            for (int i = 0; i < AMBIENT_MAX_RULES; i++) {
                s_next_us[i] = (cfg.rule[i].on && cfg.rule[i].type == 0) ? now + random_wait_us(&cfg.rule[i]) : 0;
            }
            if (media_active_slot(SLOT_BG)) media_abort_slot(SLOT_BG);  // restart with the new background
            s_bg_clip[0] = '\0';
        }
        s_running = true;

        // Background: the first enabled background rule, looping.
        const ambient_rule_t *bg = NULL;
        for (int i = 0; i < AMBIENT_MAX_RULES && !bg; i++) if (cfg.rule[i].on && cfg.rule[i].type == 1) bg = &cfg.rule[i];
        if (bg && !media_active_slot(SLOT_BG)) {
            media_set_slot_gain_db(SLOT_BG, bg->gain_db);
            if (clip_play_slot(bg->clip, true, SLOT_BG)) {
                strlcpy(s_bg_clip, bg->clip, sizeof s_bg_clip);
                ESP_LOGI(TAG, "background: %s", bg->clip);
            }
        }
        // Events.
        for (int i = 0; i < AMBIENT_MAX_RULES; i++) {
            const ambient_rule_t *r = &cfg.rule[i];
            if (!r->on || r->type != 0 || s_next_us[i] == 0 || now < s_next_us[i]) continue;
            if (media_active_slot(SLOT_EVENT)) {  // one event at a time: try again shortly
                s_next_us[i] = now + 5000000;
                continue;
            }
            media_set_slot_gain_db(SLOT_EVENT, r->gain_db);
            if (clip_play_slot(r->clip, false, SLOT_EVENT)) ESP_LOGI(TAG, "event: %s", r->clip);
            s_next_us[i] = now + random_wait_us(r);
        }
    }
}

void ambient_init(void)
{
    s_cfg = DEFAULT_CFG;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        ambient_config_t stored;
        size_t len = sizeof stored;
        if (nvs_get_blob(h, "ambient", &stored, &len) == ESP_OK && len == sizeof stored && valid(&stored)) s_cfg = stored;
        nvs_close(h);
    }
    if (safemode_active()) s_cfg.enabled = false;
    // Decoding two clips at once needs two decoder stacks, so keep the task itself small.
    xTaskCreate(ambient_task, "ambient", 4096, NULL, 3, NULL);
}

#endif  // CONFIG_AB_FEATURE_AMBIENT
