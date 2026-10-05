// HTTP endpoints for stored clips, playback control and the amp volume.
//   GET  /clips                           list of files and free space (no login)
//   POST /clips/upload?name=x.mp3         body = file (login)
//   POST /clips/play?name=x.mp3[&loop=1]  (login)
//   POST /clips/delete?name=x.mp3         (login)
//   GET  /clips/download?name=x.mp3       the file itself (login)
//   POST /clips/rename?name=a.mp3&to=b.mp3 (login)
//   POST /media/stop                      stop a clip or stream (login)
//   POST /volume?db=-40                   amp volume, capped by the speaker profile (login)
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

static const char *TAG = "clips";

static bool query_name(httpd_req_t *req, char *name, size_t len)
{
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK) return false;
    if (httpd_query_key_value(query, "name", name, len) != ESP_OK) return false;
    return clip_name_valid(name);
}

static esp_err_t bad_request(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, msg);
}

static bool in_use(const char *name)
{
    return media_kind() == MEDIA_CLIP && strcmp(media_label(), name) == 0;
}

static esp_err_t list_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"files\":[");
    DIR *d = opendir(STORAGE_PATH);
    bool first = true;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (!clip_name_valid(e->d_name)) continue;
        char path[300];
        snprintf(path, sizeof path, STORAGE_PATH "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        char item[300];
        snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"size\":%ld}", first ? "" : ",", e->d_name, (long)st.st_size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    if (d) closedir(d);
    size_t total = 0, free_b = 0;
    storage_info(&total, &free_b);
    char tail[80];
    snprintf(tail, sizeof tail, "],\"total\":%u,\"free\":%u}\n", (unsigned)total, (unsigned)free_b);
    httpd_resp_sendstr_chunk(req, tail);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char name[40];
    if (!query_name(req, name, sizeof name)) return bad_request(req, "name must be letters/digits/._- and end in .mp3 or .wav\n");
    if (in_use(name)) return bad_request(req, "that clip is playing\n");

    size_t total, free_b;
    if (!storage_info(&total, &free_b)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage not mounted");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len + 8192 > free_b) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "not enough free space\n");
    }

    char tmp[64], dst[64];
    snprintf(tmp, sizeof tmp, STORAGE_PATH "/upload.tmp");
    snprintf(dst, sizeof dst, STORAGE_PATH "/%s", name);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
        return ESP_FAIL;
    }
    static EXT_RAM_BSS_ATTR char buf[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof buf ? remaining : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) {
            fclose(f);
            remove(tmp);
            ESP_LOGE(TAG, "upload failed");
            return ESP_FAIL;
        }
        remaining -= n;
    }
    fclose(f);
    remove(dst);  // FAT cannot rename over an existing file
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "stored %s (%u bytes)", name, (unsigned)req->content_len);
    return httpd_resp_sendstr(req, "stored\n");
}

static char s_overlay[40];  // name of the clip we started on top of a stream (so Stop can end just that one)

static esp_err_t play_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char name[40];
    if (!query_name(req, name, sizeof name)) return bad_request(req, "bad name\n");
    char query[96], val[8];
    bool loop = httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
                httpd_query_key_value(query, "loop", val, sizeof val) == ESP_OK && atoi(val) != 0;
    // While a live stream (the PC's sound over the cable, VBAN, radio) owns the main channel, a clip is mixed on top of it
    // through the mixer's spare one-shot channel instead of being refused. Clips themselves still replace each other.
    if (media_active_slot(SLOT_MAIN) && media_kind_slot(SLOT_MAIN) == MEDIA_STREAM) {
        if (media_active_slot(SLOT_EVENT)) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "another clip is already playing over the stream\n");
        }
        media_set_slot_gain_db(SLOT_EVENT, 0.0f);
        if (!clip_play_slot(name, loop, SLOT_EVENT)) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "cannot play: missing file\n");
        }
        strlcpy(s_overlay, name, sizeof s_overlay);
        return httpd_resp_sendstr(req, "playing over the stream\n");
    }
    s_overlay[0] = '\0';
    if (!player_play_clip(name, loop)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "cannot play: missing file, or something else is playing\n");
    }
    return httpd_resp_sendstr(req, "playing\n");
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char name[40];
    if (!query_name(req, name, sizeof name)) return bad_request(req, "bad name\n");
    if (in_use(name)) return bad_request(req, "that clip is playing\n");
    char path[64];
    snprintf(path, sizeof path, STORAGE_PATH "/%s", name);
    if (remove(path) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no such clip\n");
    }
    return httpd_resp_sendstr(req, "deleted\n");
}

static esp_err_t download_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char name[40];
    if (!query_name(req, name, sizeof name)) return bad_request(req, "bad name\n");
    char path[64];
    snprintf(path, sizeof path, STORAGE_PATH "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no such clip\n");
    }
    char disp[80];
    snprintf(disp, sizeof disp, "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, strcasecmp(name + strlen(name) - 4, ".wav") == 0 ? "audio/wav" : "audio/mpeg");
    static char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t rename_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[120], from[40], to[40];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "name", from, sizeof from) != ESP_OK ||
        httpd_query_key_value(query, "to", to, sizeof to) != ESP_OK ||
        !clip_name_valid(from) || !clip_name_valid(to)) {
        return bad_request(req, "bad name\n");
    }
    if (strcasecmp(from + strlen(from) - 4, to + strlen(to) - 4) != 0) return bad_request(req, "keep the same file type\n");
    if (in_use(from)) return bad_request(req, "that clip is playing\n");
    char a[64], b[64];
    snprintf(a, sizeof a, STORAGE_PATH "/%s", from);
    snprintf(b, sizeof b, STORAGE_PATH "/%s", to);
    struct stat st;
    if (strcasecmp(from, to) != 0 && stat(b, &st) == 0) return bad_request(req, "a clip with that name already exists\n");
    if (rename(a, b) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "rename failed\n");
    }
    return httpd_resp_sendstr(req, "renamed\n");
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

// POST /announce  (login). Body: a short .mp3 or 16-bit PCM .wav file (up to 400 KB), optional ?type=wav|mp3 (else detected from the first bytes).
// Plays at once from memory (nothing is written to flash): on its own if the main channel is free, otherwise mixed on top of whatever plays
// (the stream is ducked meanwhile). Meant for Home Assistant text-to-speech messages.
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
    strlcpy(s_overlay, over ? "announcement" : "", sizeof s_overlay);
    return httpd_resp_sendstr(req, over ? "announcing over the current sound\n" : "announcing\n");
}

void playback_settings_load(void)
{
    nvs_handle_t h;
    uint16_t ms;
    uint8_t db;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, "prebuf_ms", &ms) == ESP_OK && ms >= 20 && ms <= 600) media_set_prebuffer_ms(ms);
        if (nvs_get_u8(h, "duck_db", &db) == ESP_OK && db <= 30) tone_set_duck_db(db);
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
        { .uri = "/clips",        .method = HTTP_GET,  .handler = list_handler },
        { .uri = "/clips/upload", .method = HTTP_POST, .handler = upload_handler },
        { .uri = "/clips/play",   .method = HTTP_POST, .handler = play_handler },
        { .uri = "/clips/delete", .method = HTTP_POST, .handler = delete_handler },
        { .uri = "/clips/download", .method = HTTP_GET, .handler = download_handler },
        { .uri = "/clips/rename", .method = HTTP_POST, .handler = rename_handler },
        { .uri = "/media/stop",   .method = HTTP_POST, .handler = stop_handler },
        { .uri = "/volume",       .method = HTTP_POST, .handler = volume_handler },
        { .uri = "/latency",      .method = HTTP_GET,  .handler = latency_get },
        { .uri = "/latency",      .method = HTTP_POST, .handler = latency_post },
        { .uri = "/duck",         .method = HTTP_GET,  .handler = duck_get },
        { .uri = "/duck",         .method = HTTP_POST, .handler = duck_post },
        { .uri = "/announce",     .method = HTTP_POST, .handler = announce_post },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
#if CONFIG_AB_FEATURE_EQ
    eq_http_register(server);
#endif
    ambient_http_register(server);  // always: it also registers the player controls
}
