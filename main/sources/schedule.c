// Sleep timer, quiet hours and alarm. See schedule.h for the rules.
#include "schedule.h"
#include "sdkconfig.h"

#if CONFIG_AB_FEATURE_SCHEDULE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ambient.h"
#include "bluetooth.h"
#include "dac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "media.h"
#include "nvs.h"
#include "ota_http.h"
#include "player.h"

static const char *TAG = "schedule";

typedef struct {
    bool quiet_on;
    uint16_t quiet_from;     // minutes after midnight
    uint16_t quiet_to;
    int8_t quiet_db;         // volume ceiling inside the window
    bool alarm_on;
    uint16_t alarm_at;
    uint8_t alarm_days;      // bit 0 = Sunday
    char alarm_clip[32];
    uint16_t alarm_s;        // how long the alarm clip loops
} sched_t;

static sched_t s = { .quiet_from = 22 * 60, .quiet_to = 7 * 60, .quiet_db = -40, .alarm_at = 7 * 60 + 30, .alarm_days = 0x3E, .alarm_s = 60 };
static int64_t s_sleep_at_us;        // 0 = no sleep timer
static int64_t s_alarm_end_us;       // 0 = alarm not sounding
static int s_alarm_day_key;          // year * 1000 + day of year of the last alarm, so it fires once per day
static bool s_in_quiet;
static bool s_reapply;               // settings changed: apply the ceiling again on the next tick
static esp_timer_handle_t s_tick;

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s;
    sched_t tmp;
    if (nvs_get_blob(h, "sched", &tmp, &len) == ESP_OK && len == sizeof tmp) s = tmp;
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "sched", &s, sizeof s);
    nvs_commit(h);
    nvs_close(h);
}

static bool clock_now(struct tm *tm)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;   // before the first network sync the clock starts in 1970
    localtime_r(&now, tm);
    return true;
}

static bool in_window(int from, int to, int now)
{
    if (from == to) return false;
    return from < to ? (now >= from && now < to) : (now >= from || now < to);   // a window may run past midnight
}

static void stop_everything(void)
{
    player_stop();
    media_abort_slot(SLOT_MAIN);
    media_abort_slot(SLOT_BG);
    media_abort_slot(SLOT_EVENT);
    ambient_config_t a = ambient_get();
    if (a.enabled) {
        a.enabled = false;
        ambient_set(&a);
    }
    bluetooth_disconnect();
}

static void start_alarm(void)
{
    if (!s.alarm_clip[0]) { ESP_LOGW(TAG, "alarm has no clip chosen"); return; }
    if (player_play_clip(s.alarm_clip, true)) {
        s_alarm_end_us = esp_timer_get_time() + (int64_t)s.alarm_s * 1000000;
        ESP_LOGI(TAG, "alarm: %s for %u s", s.alarm_clip, (unsigned)s.alarm_s);
    } else {
        ESP_LOGW(TAG, "alarm clip \"%s\" could not start (busy or missing)", s.alarm_clip);
    }
}

static void tick(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    if (s_sleep_at_us && now_us >= s_sleep_at_us) {
        s_sleep_at_us = 0;
        ESP_LOGI(TAG, "sleep timer: stopping");
        stop_everything();
    }
    if (s_alarm_end_us && now_us >= s_alarm_end_us) {
        s_alarm_end_us = 0;
        if (strcmp(player_current(), s.alarm_clip) == 0) player_stop();
    }
    struct tm tm;
    bool synced = clock_now(&tm);
    int minute = synced ? tm.tm_hour * 60 + tm.tm_min : 0;
    bool quiet = synced && s.quiet_on && in_window(s.quiet_from, s.quiet_to, minute);
    if (quiet != s_in_quiet || s_reapply) {
        s_reapply = false;
        s_in_quiet = quiet;
        dac_set_quiet_cap(quiet ? s.quiet_db : 0);
        ESP_LOGI(TAG, "quiet hours %s", quiet ? "begin" : "end");
    }
    if (synced && s.alarm_on && (s.alarm_days & (1 << tm.tm_wday)) && minute == s.alarm_at) {
        int key = (tm.tm_year + 1900) * 1000 + tm.tm_yday;
        if (key != s_alarm_day_key) {
            s_alarm_day_key = key;
            start_alarm();
        }
    }
}

void schedule_init(void)
{
    load();
    const esp_timer_create_args_t ta = { .callback = tick, .name = "schedule" };
    if (esp_timer_create(&ta, &s_tick) == ESP_OK) esp_timer_start_periodic(s_tick, 1000000);
}

// ---- web ------------------------------------------------------------------------------------------------------------

static esp_err_t send_state(httpd_req_t *req)
{
    char out[640];
    struct tm tm;
    bool synced = clock_now(&tm);
    int64_t now_us = esp_timer_get_time();
    int sleep_left = s_sleep_at_us ? (int)((s_sleep_at_us - now_us) / 1000000) : 0;
    snprintf(out, sizeof out,
             "{\"clock_ok\":%s,\"sleep_left_s\":%d,\"quiet_on\":%s,\"quiet_from\":\"%02d:%02d\",\"quiet_to\":\"%02d:%02d\",\"quiet_db\":%d,\"quiet_now\":%s,"
             "\"alarm_on\":%s,\"alarm_at\":\"%02d:%02d\",\"alarm_days\":%u,\"alarm_clip\":\"%s\",\"alarm_s\":%u,\"alarm_sounding\":%s}\n",
             synced ? "true" : "false", sleep_left < 0 ? 0 : sleep_left, s.quiet_on ? "true" : "false", s.quiet_from / 60, s.quiet_from % 60,
             s.quiet_to / 60, s.quiet_to % 60, s.quiet_db, s_in_quiet ? "true" : "false", s.alarm_on ? "true" : "false", s.alarm_at / 60, s.alarm_at % 60,
             (unsigned)s.alarm_days, s.alarm_clip, (unsigned)s.alarm_s, s_alarm_end_us ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t get_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    return send_state(req);
}

static bool parse_hhmm(const char *v, uint16_t *out)
{
    int h, m;
    if (sscanf(v, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out = (uint16_t)(h * 60 + m);
    return true;
}

static esp_err_t bad(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[400];
    if (req->content_len == 0 || req->content_len >= sizeof body) return bad(req, "body: lines like quiet_on=1\n");
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return bad(req, "read error\n");
        got += r;
    }
    body[got] = '\0';
    sched_t n = s;
    for (char *line = strtok(body, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "quiet_on")) n.quiet_on = atoi(v) != 0;
        else if (!strcmp(k, "quiet_from")) { if (!parse_hhmm(v, &n.quiet_from)) return bad(req, "quiet_from: HH:MM\n"); }
        else if (!strcmp(k, "quiet_to")) { if (!parse_hhmm(v, &n.quiet_to)) return bad(req, "quiet_to: HH:MM\n"); }
        else if (!strcmp(k, "quiet_db")) {
            int d = atoi(v);
            if (d < -90 || d > -10) return bad(req, "quiet_db: -90 to -10\n");
            n.quiet_db = (int8_t)d;
        }
        else if (!strcmp(k, "alarm_on")) n.alarm_on = atoi(v) != 0;
        else if (!strcmp(k, "alarm_at")) { if (!parse_hhmm(v, &n.alarm_at)) return bad(req, "alarm_at: HH:MM\n"); }
        else if (!strcmp(k, "alarm_days")) n.alarm_days = (uint8_t)(atoi(v) & 0x7F);
        else if (!strcmp(k, "alarm_clip")) {
            if (strlen(v) >= sizeof n.alarm_clip || strchr(v, '/')) return bad(req, "alarm_clip: a clip name\n");
            strlcpy(n.alarm_clip, v, sizeof n.alarm_clip);
        }
        else if (!strcmp(k, "alarm_s")) {
            int d = atoi(v);
            if (d < 5 || d > 1800) return bad(req, "alarm_s: 5 to 1800\n");
            n.alarm_s = (uint16_t)d;
        }
    }
    s = n;
    save();
    s_alarm_day_key = 0;    // a changed alarm time may fire again today
    s_reapply = true;   // the ceiling or the window changed
    tick(NULL);
    return send_state(req);
}

static esp_err_t sleep_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char q[24], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "min", v, sizeof v) != ESP_OK) return bad(req, "?min=N (0 cancels)\n");
    int m = atoi(v);
    if (m < 0 || m > 720) return bad(req, "min: 0 to 720\n");
    s_sleep_at_us = m ? esp_timer_get_time() + (int64_t)m * 60 * 1000000 : 0;
    return send_state(req);
}

static esp_err_t alarm_now_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    start_alarm();
    return send_state(req);
}

void schedule_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/schedule",           .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/schedule",           .method = HTTP_POST, .handler = post_handler },
        { .uri = "/schedule/sleep",     .method = HTTP_POST, .handler = sleep_handler },
        { .uri = "/schedule/alarm_now", .method = HTTP_POST, .handler = alarm_now_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#else  // feature switched off at build time

void schedule_init(void) {}
void schedule_http_register(httpd_handle_t server) { (void)server; }

#endif
