#include "ota_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bootinfo.h"
#include "dac.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "speaker_limits.h"
#include "media.h"
#include "features.h"
#include "ambient.h"
#include "esp_heap_caps.h"
#include "net.h"
#include "player.h"
#include "safemode.h"
#include "storage.h"
#include "nvs.h"
#include "tone.h"

static const char *TAG = "http";

#if CONFIG_AB_FEATURE_AMBIENT
#define AMBIENT_ON() (ambient_get().enabled)
#define AMBIENT_RUNNING() ambient_running()
#else
#define AMBIENT_ON() false
#define AMBIENT_RUNNING() false
#endif
#define FEAT(x) ((x) ? "true" : "false")

// ---- credentials and sessions ------------------------------------------------------------

#define PASS_MAX      63
#define MAX_SESSIONS  4
#define SID_LEN       32  // hex characters
#define COOKIE_MAX_AGE (30 * 24 * 3600)

static char s_pass[PASS_MAX + 1];                  // password; also accepted as X-Token header
static char s_sessions[MAX_SESSIONS][SID_LEN + 1]; // valid session ids (persisted in NVS)
static int s_next_session;
static int s_fail_count;
static int64_t s_locked_until_us;

static void to_hex(char *dst, const uint8_t *raw, size_t n)
{
    for (size_t i = 0; i < n; i++) sprintf(&dst[2 * i], "%02x", raw[i]);
}

static void save_sessions(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "sessions", s_sessions, sizeof s_sessions);
    nvs_commit(h);
    nvs_close(h);
}

static void load_credentials(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("audiobrick", NVS_READWRITE, &h));
    size_t len = sizeof s_pass;
    // The key stays "token" so existing installs keep their password.
    if (nvs_get_str(h, "token", s_pass, &len) != ESP_OK || s_pass[0] == '\0') {
        uint8_t raw[16];
        esp_fill_random(raw, sizeof raw);
        to_hex(s_pass, raw, sizeof raw);
        ESP_ERROR_CHECK(nvs_set_str(h, "token", s_pass));
        ESP_ERROR_CHECK(nvs_commit(h));
        ESP_LOGW(TAG, "generated a new initial password");
    }
    len = sizeof s_sessions;
    if (nvs_get_blob(h, "sessions", s_sessions, &len) != ESP_OK || len != sizeof s_sessions) {
        memset(s_sessions, 0, sizeof s_sessions);
    }
    nvs_close(h);
    s_sessions[0][SID_LEN] = s_sessions[1][SID_LEN] = s_sessions[2][SID_LEN] = s_sessions[3][SID_LEN] = '\0';
    ESP_LOGW(TAG, "web password (change it on the settings page): %s", s_pass);
}

// Constant-time string comparison (length differences are not hidden; they are not secret here).
static bool secure_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la != lb;
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static bool cookie_sid(httpd_req_t *req, char *out, size_t out_len)
{
    char hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof hdr) != ESP_OK) return false;
    for (char *p = hdr; (p = strstr(p, "sid=")) != NULL; p += 4) {
        if (p != hdr && p[-1] != ' ' && p[-1] != ';') continue;
        p += 4;
        size_t n = strcspn(p, "; ");
        if (n == 0 || n >= out_len) return false;
        memcpy(out, p, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

static bool session_valid(const char *sid)
{
    bool ok = false;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i][0] != '\0' && secure_eq(sid, s_sessions[i])) ok = true;
    }
    return ok;
}

static void drop_session(const char *sid)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i][0] != '\0' && secure_eq(sid, s_sessions[i])) s_sessions[i][0] = '\0';
    }
    save_sessions();
}

static void new_session(char *sid_out)
{
    uint8_t raw[SID_LEN / 2];
    esp_fill_random(raw, sizeof raw);
    to_hex(sid_out, raw, sizeof raw);
    memcpy(s_sessions[s_next_session], sid_out, SID_LEN + 1);
    s_next_session = (s_next_session + 1) % MAX_SESSIONS;  // oldest session is replaced
    save_sessions();
}

static bool authorized(httpd_req_t *req)
{
    char given[PASS_MAX + 8];
    if (httpd_req_get_hdr_value_str(req, "X-Token", given, sizeof given) == ESP_OK) {
        return secure_eq(given, s_pass);
    }
    char sid[SID_LEN + 8];
    return cookie_sid(req, sid, sizeof sid) && session_valid(sid);
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_sendstr(req, "please log in\n");
}

bool web_authorized(httpd_req_t *req) { return authorized(req); }
esp_err_t web_deny(httpd_req_t *req) { return deny(req); }
bool web_password_ok(const char *candidate) { return secure_eq(candidate, s_pass); }

static int read_body(httpd_req_t *req, char *buf, size_t max)
{
    if (req->content_len == 0 || req->content_len >= max) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return -1;
        got += n;
    }
    buf[got] = '\0';
    return (int)got;
}

static esp_err_t send_cookie_ok(httpd_req_t *req, const char *sid)
{
    static char cookie[96];
    snprintf(cookie, sizeof cookie, "sid=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict", sid, COOKIE_MAX_AGE);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}\n");
}

static esp_err_t login_handler(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until_us) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, "too many attempts, wait 30 s\n");
    }
    char body[PASS_MAX + 8];
    if (read_body(req, body, sizeof body) < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad request\n");
    }
    if (!secure_eq(body, s_pass)) {
        if (++s_fail_count >= 5) {
            s_fail_count = 0;
            s_locked_until_us = esp_timer_get_time() + 30LL * 1000000;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));  // slow down guessing
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "wrong password\n");
    }
    s_fail_count = 0;
    char sid[SID_LEN + 1];
    new_session(sid);
    return send_cookie_ok(req, sid);
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    char sid[SID_LEN + 8];
    if (cookie_sid(req, sid, sizeof sid)) drop_session(sid);
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return httpd_resp_sendstr(req, "{\"ok\":true}\n");
}

static esp_err_t session_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, authorized(req) ? "{\"logged_in\":true}\n" : "{\"logged_in\":false}\n");
}

static esp_err_t password_handler(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    char body[PASS_MAX + 8];
    int n = read_body(req, body, sizeof body);
    bool ok = n >= 8 && n <= PASS_MAX;
    for (int i = 0; ok && i < n; i++) ok = body[i] >= 33 && body[i] <= 126;  // printable, no spaces
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "password must be 8-63 printable characters, no spaces\n");
    }
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage error");
        return ESP_FAIL;
    }
    bool saved = nvs_set_str(h, "token", body) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!saved) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage error");
        return ESP_FAIL;
    }
    strcpy(s_pass, body);
    // Log everyone else out; the caller gets a fresh session.
    memset(s_sessions, 0, sizeof s_sessions);
    char sid[SID_LEN + 1];
    new_session(sid);
    return send_cookie_ok(req, sid);
}

// ---- read-only pages -----------------------------------------------------------------------

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");  // always pick up a firmware update's new page
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();
    tone_state_t t = tone_get();
    // 0x70 CHAN_FAULT, 0x71 GLOBAL_FAULT1, 0x72 GLOBAL_FAULT2, 0x73 WARNING (0 if unreadable)
    uint8_t reg[4] = { 0 };
    for (int i = 0; i < 4; i++) dac_read_reg(0x70 + i, &reg[i]);
    float pvdd = dac_pvdd_volts();
    char clock_str[24] = "";
    bool clock_ok = false;
    {
        time_t now = time(NULL);
        if (now > 1700000000) {  // before the first sync the clock starts in 1970
            struct tm tm;
            localtime_r(&now, &tm);
            strftime(clock_str, sizeof clock_str, "%Y-%m-%d %H:%M:%S", &tm);
            clock_ok = true;
        }
    }
    int vol = dac_get_volume_db();
    float vrms, watts, spl;
    limits_estimate(t.level_dbfs, (float)vol, pvdd, &vrms, &watts, &spl);
    float pk_v, pk_w, pk_spl;  // the loudest possible peak at this volume: a full-scale (0 dBFS) signal
    limits_estimate(0.0f, (float)vol, pvdd, &pk_v, &pk_w, &pk_spl);
    // True when the requested peak is above what the supply can deliver, so the output clips.
    bool clip = t.enabled && pvdd > 0.0f &&
                FULLSCALE_VPK * powf(10.0f, (t.level_dbfs + (float)vol) / 20.0f) > 0.9f * pvdd;
    media_kind_t mk = media_kind();
    size_t st_total = 0, st_free = 0;
    storage_info(&st_total, &st_free);
    char json[2600];
    snprintf(json, sizeof json,
             "{\"version\":\"%s\",\"built\":\"%s %s\",\"partition\":\"%s\",\"uptime_s\":%lld,\"ip\":\"%s\","
             "\"boot\":{\"reason\":\"%s\",\"abnormal\":%s,\"count\":%u,\"crashes\":%u},\"time\":\"%s\",\"time_synced\":%s,\"pvdd_v\":%.2f,\"fault\":%s,\"warning\":%s,"
             "\"regs\":{\"chan_fault\":%u,\"fault1\":%u,\"fault2\":%u,\"warning\":%u},"
             "\"amp\":\"%s\",\"safe_mode\":%s,\"vol_db\":%d,\"volume_level\":%.3f,\"clip\":%s,"
             "\"tone\":{\"on\":%s,\"freq_hz\":%.1f,\"db\":%.1f},"
             "\"est\":{\"vrms\":%.3f,\"watts\":%.3f,\"spl\":%.1f},"
             "\"est_peak\":{\"vrms\":%.3f,\"watts\":%.3f,\"spl\":%.1f},"
             "\"media\":{\"src\":\"%s\",\"label\":\"%s\",\"buffer_ms\":%u,\"underruns\":%u,\"overlay\":\"%s\"},"
             "\"storage\":{\"total\":%u,\"free\":%u},"
             "\"player\":{\"state\":\"%s\",\"clip\":\"%s\",\"index\":%d,\"count\":%d},"
             "\"ambient\":{\"on\":%s,\"running\":%s,\"bg\":\"%s\",\"event\":\"%s\"},"
             "\"heap\":{\"free\":%u,\"largest\":%u,\"min\":%u},"
             "\"features\":{\"eq\":%s,\"ambient\":%s,\"vban\":%s,\"scream\":%s,\"radio\":%s,\"synth\":%s,\"bluetooth\":%s,\"clips\":%s,\"announce\":%s,\"tcpstream\":%s,\"powersave\":%s,\"sntp\":%s,\"safemode\":%s,\"bootinfo\":%s,\"mdns\":%s,\"wifi\":%s,\"schedule\":%s}}\n",
             app->version, app->date, app->time, part ? part->label : "?",
             (long long)(esp_timer_get_time() / 1000000), net_ip_str(), bootinfo_reason(), bootinfo_abnormal() ? "true" : "false", (unsigned)bootinfo_count(), (unsigned)bootinfo_crashes(), clock_str, clock_ok ? "true" : "false", pvdd,
             dac_fault_active() ? "true" : "false", dac_warning_active() ? "true" : "false",
             reg[0], reg[1], reg[2], reg[3], dac_state() == AMP_OFF ? "off" : (dac_state() == AMP_HIZ ? "hiz" : "active"), safemode_active() ? "true" : "false", vol, (vol + 70.0f) / (limits_max_volume_db() + 70.0f), clip ? "true" : "false",
             t.enabled ? "true" : "false", t.freq_hz, t.level_dbfs,
             vrms, watts, spl,
             pk_v, pk_w, pk_spl,
             mk == MEDIA_STREAM ? "stream" : (mk == MEDIA_CLIP ? "clip" : "none"), media_label(),
             (unsigned)media_buffer_ms(), (unsigned)media_underruns(),
             (media_active_slot(SLOT_MAIN) && media_active_slot(SLOT_EVENT)) ? media_label_slot(SLOT_EVENT) : "",  // a clip mixed over a stream
             (unsigned)st_total, (unsigned)st_free,
             player_state() == PLAYER_PLAYING ? "playing" : (player_state() == PLAYER_PAUSED ? "paused" : "idle"),
             player_current(), player_index(), player_count(),
             AMBIENT_ON() ? "true" : "false", AMBIENT_RUNNING() ? "true" : "false",
             media_active_slot(SLOT_BG) ? media_label_slot(SLOT_BG) : "", media_active_slot(SLOT_EVENT) ? media_label_slot(SLOT_EVENT) : "",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             FEAT(AB_HAS_EQ), FEAT(AB_HAS_AMBIENT), FEAT(AB_HAS_VBAN), FEAT(AB_HAS_SCREAM),
             FEAT(AB_HAS_RADIO), FEAT(AB_HAS_SYNTH), FEAT(AB_HAS_BLUETOOTH),
             FEAT(AB_HAS_CLIPS), FEAT(AB_HAS_ANNOUNCE), FEAT(AB_HAS_TCPSTREAM), FEAT(AB_HAS_POWERSAVE), FEAT(AB_HAS_SNTP),
             FEAT(AB_HAS_SAFEMODE), FEAT(AB_HAS_BOOTINFO), FEAT(AB_HAS_MDNS), FEAT(AB_HAS_WIFI), FEAT(AB_HAS_SCHEDULE));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t profile_get_handler(httpd_req_t *req)
{
    speaker_profile_t p = limits_get();
    char json[360];
    snprintf(json, sizeof json,
             "{\"name\":\"%s\",\"ohms\":%.2f,\"sens\":%.1f,\"dist\":%.2f,\"maxspl\":%.0f,"
             "\"ratedw\":%.1f,\"minhz\":%.0f,\"max_vrms\":%.3f,\"max_vol_db\":%d,\"fs_vpk\":%.1f}\n",
             p.name, p.ohms, p.sens_db, p.dist_m, p.max_spl_db, p.rated_w, p.min_hz,
             limits_max_vrms(), limits_max_volume_db(), FULLSCALE_VPK);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// ---- settings and tests (login required) -----------------------------------------------------

// Names are limited to letters, digits, space, '-' and '_' so they are safe to echo in JSON.
static void sanitize_name(char *dst, size_t dst_len, const char *src)
{
    size_t n = 0;
    for (; *src && n + 1 < dst_len; src++) {
        char c = *src == '+' ? ' ' : *src;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == ' ' || c == '-' || c == '_';
        dst[n++] = ok ? c : '_';
    }
    dst[n] = '\0';
}

static esp_err_t faults_clear_handler(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);
    dac_clear_faults();
    return status_handler(req);
}

static esp_err_t profile_post_handler(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);

    speaker_profile_t p = limits_get();
    char query[256], val[48];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing query\n");
    }
    if (httpd_query_key_value(query, "name", val, sizeof val) == ESP_OK) sanitize_name(p.name, sizeof p.name, val);
    if (httpd_query_key_value(query, "ohms", val, sizeof val) == ESP_OK) p.ohms = strtof(val, NULL);
    if (httpd_query_key_value(query, "sens", val, sizeof val) == ESP_OK) p.sens_db = strtof(val, NULL);
    if (httpd_query_key_value(query, "dist", val, sizeof val) == ESP_OK) p.dist_m = strtof(val, NULL);
    if (httpd_query_key_value(query, "maxspl", val, sizeof val) == ESP_OK) p.max_spl_db = strtof(val, NULL);
    if (httpd_query_key_value(query, "ratedw", val, sizeof val) == ESP_OK) p.rated_w = strtof(val, NULL);
    if (httpd_query_key_value(query, "minhz", val, sizeof val) == ESP_OK) p.min_hz = strtof(val, NULL);

    if (!limits_set(&p)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "profile rejected: value out of range\n");
    }
    // Re-apply the caps under the new profile.
    dac_set_volume_db(dac_get_volume_db());
    tone_state_t t = tone_get();
    tone_set(t.enabled, t.freq_hz, t.level_dbfs);
    return profile_get_handler(req);
}

static esp_err_t tone_handler(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);

    tone_state_t cur = tone_get();
    bool on = cur.enabled;
    float freq = cur.freq_hz, db = cur.level_dbfs;
    int ttl = TONE_DEFAULT_TTL_S;  // dead-man timer; refresh by repeating the request

    char query[96], val[24];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        if (httpd_query_key_value(query, "on", val, sizeof val) == ESP_OK) on = atoi(val) != 0;
        if (httpd_query_key_value(query, "freq", val, sizeof val) == ESP_OK) freq = strtof(val, NULL);
        if (httpd_query_key_value(query, "db", val, sizeof val) == ESP_OK) db = strtof(val, NULL);
        // Optional amp digital volume in dB; capped by the speaker profile.
        if (httpd_query_key_value(query, "vol", val, sizeof val) == ESP_OK) dac_set_volume_db(atoi(val));
        if (httpd_query_key_value(query, "ttl", val, sizeof val) == ESP_OK) ttl = atoi(val);
    }
    if (on && media_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "stop the stream or clip first\n");
    }
    // Refuse to start the tone while the amp reports a fault.
    if (on && dac_fault_active()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "amp fault active\n");
    }
    if (on) tone_hold(ttl);
    tone_set(on, freq, db);
    return status_handler(req);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t update_handler(httpd_req_t *req)
{
    if (!authorized(req)) return deny(req);

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > next->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad firmware size");
        return ESP_FAIL;
    }

    // No sound while flashing; the tone task mutes the amp once the fade-out completes.
    tone_set(false, tone_get().freq_hz, tone_get().level_dbfs);

    esp_ota_handle_t ota;
    if (esp_ota_begin(next, req->content_len, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: %d bytes into %s", req->content_len, next->label);

    static EXT_RAM_BSS_ATTR char buf[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof buf ? remaining : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "OTA: receive failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image rejected");
            return ESP_FAIL;
        }
        remaining -= n;
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(next) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image validation failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: done, rebooting");
    httpd_resp_sendstr(req, "OK, rebooting\n");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

bool ota_http_start(void)
{
    load_credentials();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 20;
    cfg.max_uri_handlers = 64;
    cfg.max_open_sockets = 8;  // the lwIP pool is 16: web server 8 + 2 internal, the stream port and its client
    cfg.lru_purge_enable = true;
    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/status",   .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/profile",  .method = HTTP_GET,  .handler = profile_get_handler },
        { .uri = "/session",  .method = HTTP_GET,  .handler = session_handler },
        { .uri = "/login",    .method = HTTP_POST, .handler = login_handler },
        { .uri = "/logout",   .method = HTTP_POST, .handler = logout_handler },
        { .uri = "/password", .method = HTTP_POST, .handler = password_handler },
        { .uri = "/profile",  .method = HTTP_POST, .handler = profile_post_handler },
        { .uri = "/tone",     .method = HTTP_POST, .handler = tone_handler },
        { .uri = "/faults/clear", .method = HTTP_POST, .handler = faults_clear_handler },
        { .uri = "/update",   .method = HTTP_POST, .handler = update_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ota_http_register_more(server);
    return true;
}

void ota_mark_valid(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
}
