#include "player.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "clip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "storage.h"

static const char *TAG = "player";

#define MAX_CLIPS 64

static char s_current[32];
static bool s_loop;
static volatile bool s_autonext;
static volatile bool s_user_stopped;  // set when the clip was stopped on purpose, so auto-advance stays quiet

static int cmp_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

// Fills names (32 bytes each) with the sorted clip names; returns how many.
static int list_clips(char names[][32], int max)
{
    int n = 0;
    DIR *d = opendir(STORAGE_PATH);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n < max) {
        if (!clip_name_valid(e->d_name)) continue;
        strlcpy(names[n++], e->d_name, 32);
    }
    if (d) closedir(d);
    qsort(names, n, 32, cmp_names);
    return n;
}

static bool start(const char *name, bool loop)
{
    if (media_active_slot(SLOT_MAIN)) {
        s_user_stopped = true;  // replacing the clip is not "ended by itself"
        media_abort_slot(SLOT_MAIN);
        for (int i = 0; i < 40 && media_active_slot(SLOT_MAIN); i++) vTaskDelay(pdMS_TO_TICKS(25));
    }
    s_user_stopped = false;
    if (!clip_play_slot(name, loop, SLOT_MAIN)) return false;
    strlcpy(s_current, name, sizeof s_current);
    s_loop = loop;
    return true;
}

bool player_play_clip(const char *name, bool loop) { return start(name, loop); }

bool player_play(void)
{
    if (media_active_slot(SLOT_MAIN) && media_paused()) {
        media_set_paused(false);
        return true;
    }
    if (media_active_slot(SLOT_MAIN)) return true;  // already playing
    if (s_current[0]) return start(s_current, s_loop);
    static char names[MAX_CLIPS][32];
    int n = list_clips(names, MAX_CLIPS);
    return n > 0 && start(names[0], false);
}

bool player_pause(void)
{
    if (!media_active_slot(SLOT_MAIN)) return false;
    if (strncmp(media_label_slot(SLOT_MAIN), "radio:", 6) == 0) {  // live stream: pausing would resume with stale audio
        s_user_stopped = true;
        media_abort_slot(SLOT_MAIN);
        return true;
    }
    media_set_paused(true);
    return true;
}

static bool step(int dir)
{
    if (media_kind_slot(SLOT_MAIN) == MEDIA_STREAM) return false;
    static char names[MAX_CLIPS][32];
    int n = list_clips(names, MAX_CLIPS);
    if (n == 0) return false;
    int idx = -1;
    for (int i = 0; i < n; i++) if (strcasecmp(names[i], s_current) == 0) idx = i;
    idx = idx < 0 ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
    return start(names[idx], false);
}

bool player_next(void) { return step(+1); }
bool player_prev(void) { return step(-1); }

bool player_stop(void)
{
    s_user_stopped = true;
    media_abort_slot(SLOT_MAIN);
    return true;
}

player_state_t player_state(void)
{
    if (!media_active_slot(SLOT_MAIN)) return PLAYER_IDLE;
    return media_paused() ? PLAYER_PAUSED : PLAYER_PLAYING;
}

const char *player_current(void)
{
    if (media_kind_slot(SLOT_MAIN) == MEDIA_STREAM) return media_label_slot(SLOT_MAIN);
    return s_current;
}

int player_count(void)
{
    static char names[MAX_CLIPS][32];
    return list_clips(names, MAX_CLIPS);
}

int player_index(void)
{
    static char names[MAX_CLIPS][32];
    int n = list_clips(names, MAX_CLIPS);
    for (int i = 0; i < n; i++) if (strcasecmp(names[i], s_current) == 0) return i + 1;
    return 0;
}

void player_set_autonext(bool on) { s_autonext = on; }
bool player_autonext(void) { return s_autonext; }

// Watches the main channel: when a clip finishes by itself and auto-advance is on, play the next one.
static void player_task(void *arg)
{
    bool was_clip = false;
    while (true) {
        bool clip_now = media_active_slot(SLOT_MAIN) && media_kind_slot(SLOT_MAIN) == MEDIA_CLIP;
        if (was_clip && !clip_now && s_autonext && !s_user_stopped) {
            ESP_LOGI(TAG, "clip ended, advancing");
            step(+1);
        }
        was_clip = clip_now;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void player_init(void)
{
    xTaskCreate(player_task, "player", 4096, NULL, 3, NULL);
}
