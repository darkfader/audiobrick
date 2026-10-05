// HTTP endpoints for playback control that every build has: amp volume, stream delay, stop, and (optional) announcements and ducking.
//   POST /volume?level=0..1|db=N      amp volume, never above the speaker profile's cap (login)
//   GET/POST /latency                 how much audio is collected before a stream or clip starts (login to change)
//   POST /media/stop                  stop what plays on the main channel (login)
//   GET/POST /duck, POST /announce    only with AB_FEATURE_ANNOUNCE
#include "sdkconfig.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "clip.h"
#include "dac.h"
#include "esp_http_server.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "speaker_limits.h"
#include "media.h"
#include "ota_http.h"
#include "player.h"
#include "storage.h"
#include "esp_heap_caps.h"
#include "tone.h"

#include "playback_http.h"

static esp_err_t bad_request(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, msg);
}

static char s_overlay[40];  // name of the clip/announcement we started on top of a stream (so Stop can end just that one)

void playback_overlay_set(const char *name)
{
    strlcpy(s_overlay, name ? name : "", sizeof s_overlay);
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    media_abort();
    // an overlay clip is ours to stop too, but never touch the ambient scene's own one-shots
    if (s_overlay[0] && media_active_slot(SLOT_EVENT) && strcmp(media_label_slot(SLOT_EVENT), s_overlay) == 0) {
        media_abort_slot(SLOT_EVENT);
    }
    s_overlay[0] = '\0';
    return httpd_resp_sendstr(req, "stopped\n");
}

// GET /latency -> {"prebuffer_ms":170}   POST /latency?ms=50 (login): how much audio is collected before playback starts.
static esp_err_t latency_get(httpd_req_t *req)
{
    char out[48];
    snprintf(out, sizeof out, "{\"prebuffer_ms\":%u}\n", (unsigned)media_prebuffer_ms());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t latency_post(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "ms", val, sizeof val) != ESP_OK) {
        return bad_request(req, "missing ms (20-600)\n");
    }
    int ms = atoi(val);
    if (ms < 20 || ms > 600) return bad_request(req, "ms must be 20-600\n");
    media_set_prebuffer_ms((uint32_t)ms);
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "prebuf_ms", (uint16_t)ms);
        nvs_commit(h);
        nvs_close(h);
    }
    return latency_get(req);
}

#if CONFIG_AB_FEATURE_ANNOUNCE
// GET /duck -> {"duck_db":12}   POST /duck?db=12 (login, 0-30, 0 = off): how far a stream is lowered while a clip or announcement plays over it.
static esp_err_t duck_get(httpd_req_t *req)
{
    char out[32];
    snprintf(out, sizeof out, "{\"duck_db\":%d}\n", tone_duck_db());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t duck_post(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[24], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "db", val, sizeof val) != ESP_OK) {
        return bad_request(req, "missing db (0-30)\n");
    }
    int db = atoi(val);
    if (db < 0 || db > 30) return bad_request(req, "db must be 0-30\n");
    tone_set_duck_db(db);
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "duck_db", (uint8_t)db);
        nvs_commit(h);
        nvs_close(h);
    }
    return duck_get(req);
}

#define ANNOUNCE_MAX (400 * 1024)
static esp_err_t announce_post(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    if (req->content_len < 64) return bad_request(req, "send the sound file as the request body\n");
    if (req->content_len > ANNOUNCE_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "announcement too large (max 400 KB)\n");
    }
    bool over = media_active_slot(SLOT_MAIN);
    int slot = over ? SLOT_EVENT : SLOT_MAIN;
    if (media_active_slot(slot) || tone_get().enabled) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "busy: another announcement or the test tone is playing\n");
    }
    uint8_t *buf = heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "out of memory\n");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    bool is_wav = memcmp(buf, "RIFF", 4) == 0;
    char query[24], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "type", val, sizeof val) == ESP_OK) {
        is_wav = strcasecmp(val, "wav") == 0;
    }
    if (!clip_play_memory(buf, got, is_wav, "announcement", slot)) {  // takes over (and frees) buf
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "cannot start: channel busy\n");
    }
    playback_overlay_set(over ? "announcement" : "");
    return httpd_resp_sendstr(req, over ? "announcing over the current sound\n" : "announcing\n");
}
#endif  // CONFIG_AB_FEATURE_ANNOUNCE

void playback_settings_load(void)
{
    nvs_handle_t h;
    uint16_t ms;
    uint8_t db;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, "prebuf_ms", &ms) == ESP_OK && ms >= 20 && ms <= 600) media_set_prebuffer_ms(ms);
        #if CONFIG_AB_FEATURE_ANNOUNCE
        if (nvs_get_u8(h, "duck_db", &db) == ESP_OK && db <= 30) tone_set_duck_db(db);
#endif
        nvs_close(h);
    }
}

static esp_err_t volume_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[48], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK) return bad_request(req, "missing db or level\n");
    if (httpd_query_key_value(query, "level", val, sizeof val) == ESP_OK) {
        // level 0..1 maps from -70 dB up to the speaker profile's cap (what a media player slider expects)
        float lv = strtof(val, NULL);
        if (!(lv >= 0.0f)) lv = 0.0f;
        if (lv > 1.0f) lv = 1.0f;
        dac_set_volume_db((int)(-70.0f + lv * (limits_max_volume_db() + 70.0f) + 0.5f));
    } else if (httpd_query_key_value(query, "db", val, sizeof val) == ESP_OK) {
        dac_set_volume_db(atoi(val));  // clamped to the speaker profile's cap
    } else {
        return bad_request(req, "missing db or level\n");
    }
    char out[40];
    snprintf(out, sizeof out, "{\"vol_db\":%d}\n", dac_get_volume_db());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

void ota_http_register_more(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/media/stop",   .method = HTTP_POST, .handler = stop_handler },
        { .uri = "/volume",       .method = HTTP_POST, .handler = volume_handler },
        { .uri = "/latency",      .method = HTTP_GET,  .handler = latency_get },
        { .uri = "/latency",      .method = HTTP_POST, .handler = latency_post },
#if CONFIG_AB_FEATURE_ANNOUNCE
        { .uri = "/duck",         .method = HTTP_GET,  .handler = duck_get },
        { .uri = "/duck",         .method = HTTP_POST, .handler = duck_post },
        { .uri = "/announce",     .method = HTTP_POST, .handler = announce_post },
#endif
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
#if CONFIG_AB_FEATURE_CLIPS
    clips_http_register(server);
#endif
#if CONFIG_AB_FEATURE_EQ
    eq_http_register(server);
#endif
    ambient_http_register(server);  // always: it registers the optional feature modules' endpoints
}
