#include "radio.h"

#if CONFIG_AB_FEATURE_RADIO


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "media.h"
#include "nvs.h"
#include "ota_http.h"
#include "safemode.h"

#define MINIMP3_ONLY_MP3
#include "minimp3.h"

// Core 0 belongs to the Wi-Fi and Bluetooth stacks, which starve a decoder that lands there (the clip then stutters, measured with Wi-Fi on).
// Core 1 only runs the audio mixer.
#define DECODER_CORE 1

// Decoder work buffers are big (about 28 KB per decoder) and internal RAM is scarce: take them from PSRAM, fall back to internal RAM if that fails.
static void *work_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
}

static const char *TAG = "radio";

#define IN_BUF 8192

static radio_preset_t s_presets[RADIO_PRESETS];
static char s_url[160];
static char s_name[24];
static char s_status[64] = "stopped";
static volatile bool s_running;

static const radio_preset_t DEFAULTS[RADIO_PRESETS] = {
    { "Radio Paradise", "http://stream.radioparadise.com/mp3-128" },
    { "KEXP Seattle", "http://kexp-mp3-128.streamguys1.com/kexp128.mp3" },
};

bool radio_running(void) { return s_running; }
const char *radio_status(void) { return s_status; }

static bool url_ok(const char *u)
{
    size_t n = strlen(u);
    if (n < 10 || n >= sizeof s_url) return false;
    if (strncasecmp(u, "http://", 7) != 0 && strncasecmp(u, "https://", 8) != 0) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)u[i] <= 32 || (unsigned char)u[i] > 126) return false;
    return true;
}

static bool name_ok(const char *s)
{
    for (; *s; s++) if (*s < 32 || *s > 126 || *s == '"' || *s == '\\' || *s == '|') return false;
    return true;
}

// ---- playback ---------------------------------------------------------------------------------

typedef struct {
    resampler_t rs;
    uint32_t cur_rate;
    int16_t stereo[2 * 256];
    int16_t out[2 * 2048];
} rctx_t;

static bool push(rctx_t *c, const int16_t *pcm, size_t frames, int channels, uint32_t rate)
{
    if (rate != c->cur_rate) {
        resampler_init(&c->rs, rate);
        c->cur_rate = rate;
    }
    while (frames > 0) {
        size_t n = frames < 256 ? frames : 256;
        for (size_t i = 0; i < n; i++) {
            c->stereo[2 * i] = pcm[channels * i];
            c->stereo[2 * i + 1] = channels > 1 ? pcm[channels * i + 1] : pcm[channels * i];
        }
        size_t produced = n;
        const int16_t *src = c->stereo;
        if (rate != 48000) {
            produced = resampler_process(&c->rs, c->stereo, n, c->out, 2048);
            src = c->out;
        }
        if (produced && media_write(src, produced) < produced) return false;  // stopped
        pcm += channels * n;
        frames -= n;
    }
    return true;
}

typedef enum { RESULT_STOPPED, RESULT_RETRY, RESULT_FATAL } result_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy((char *)evt->user_data, evt->header_value, 64);
    }
    return ESP_OK;
}

// One connection: returns why it ended.
static result_t play_once(rctx_t *ctx, bool *got_audio)
{
    char ctype[64] = "";
    esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "AudioBrick/1.0",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
        .event_handler = on_http_event,
        .user_data = ctype,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(s_status, sizeof s_status, "out of memory");
        return RESULT_RETRY;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "0");  // no in-band song titles, they would sound like noise
    result_t result = RESULT_RETRY;
    mp3dec_t *dec = work_alloc(sizeof *dec);
    uint8_t *in = work_alloc(IN_BUF);
    int16_t *pcm = work_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!dec || !in || !pcm) {
        snprintf(s_status, sizeof s_status, "out of memory");
        goto done;
    }
    mp3dec_init(dec);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(s_status, sizeof s_status, "cannot connect");
        goto done;
    }
    esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);
    if (code != 200) {
        snprintf(s_status, sizeof s_status, "server answered %d", code);
        result = (code >= 400 && code < 500) ? RESULT_FATAL : RESULT_RETRY;
        goto done;
    }
    const char *ct = ctype[0] ? ctype : NULL;
    if (ct && !strcasestr(ct, "mpeg") && !strcasestr(ct, "mp3") && !strcasestr(ct, "octet-stream")) {
        snprintf(s_status, sizeof s_status, "not MP3 (%.40s)", ct);
        result = RESULT_FATAL;
        goto done;
    }
    snprintf(s_status, sizeof s_status, "buffering");

    size_t in_len = 0;
    bool announced = false;
    while (!media_aborted()) {
        if (in_len < IN_BUF) {
            int r = esp_http_client_read(client, (char *)in + in_len, IN_BUF - in_len);
            if (r < 0) {  // timeout or connection lost
                snprintf(s_status, sizeof s_status, "connection lost");
                break;
            }
            if (r == 0 && in_len == 0) {
                snprintf(s_status, sizeof s_status, announced ? "stream ended" : "no MP3 audio in this stream");
                if (!announced) result = RESULT_FATAL;
                goto done;
            }
            in_len += r;
        }
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, in, (int)in_len, pcm, &info);
        if (info.frame_bytes == 0) {  // not enough data for a frame yet
            if (in_len >= IN_BUF) in_len = 0;  // garbage: resynchronise
            continue;
        }
        memmove(in, in + info.frame_bytes, in_len - info.frame_bytes);
        in_len -= info.frame_bytes;
        if (samples > 0) {
            if (!announced) {
                announced = true;
                *got_audio = true;
                snprintf(s_status, sizeof s_status, "playing (%d kbps)", info.bitrate_kbps);
            }
            if (!push(ctx, pcm, samples, info.channels, info.hz)) {
                result = RESULT_STOPPED;
                goto done;
            }
        }
    }
    if (media_aborted()) result = RESULT_STOPPED;
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(dec);
    free(in);
    free(pcm);
    return result;
}

static void radio_task(void *arg)
{
    rctx_t *ctx = calloc(1, sizeof *ctx);
    int failures = 0;
    if (!ctx) snprintf(s_status, sizeof s_status, "out of memory");
    while (ctx && !media_aborted()) {
        bool got_audio = false;
        result_t r = play_once(ctx, &got_audio);
        if (r != RESULT_RETRY) break;
        failures = got_audio ? 0 : failures + 1;
        if (failures >= 5) {
            ESP_LOGW(TAG, "giving up: %s", s_status);
            break;
        }
        char prev[40];
        strlcpy(prev, s_status, sizeof prev);
        snprintf(s_status, sizeof s_status, "reconnecting (%s)", prev);
        for (int i = 0; i < 30 && !media_aborted(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (media_aborted()) snprintf(s_status, sizeof s_status, "stopped");
    free(ctx);
    s_running = false;
    media_finish();
    vTaskDeleteWithCaps(NULL);
}

bool radio_play(const char *url, const char *name)
{
    if (!url_ok(url) || !name_ok(name)) return false;
    if (media_active()) {  // replace whatever is on the main channel
        media_abort();
        for (int i = 0; i < 60 && media_active(); i++) vTaskDelay(pdMS_TO_TICKS(25));
    }
    char label[40];
    snprintf(label, sizeof label, "radio: %.28s", name[0] ? name : "stream");
    if (!media_begin(MEDIA_STREAM, label)) return false;
    strlcpy(s_url, url, sizeof s_url);
    strlcpy(s_name, name, sizeof s_name);
    snprintf(s_status, sizeof s_status, "connecting");
    s_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(radio_task, "radio", 28672, NULL, 4, NULL, DECODER_CORE, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_running = false;
        media_abort();
        media_finish();
        return false;
    }
    return true;
}

bool radio_play_preset(int i)
{
    if (i < 0 || i >= RADIO_PRESETS || s_presets[i].url[0] == '\0') return false;
    return radio_play(s_presets[i].url, s_presets[i].name);
}

// ---- stations and HTTP --------------------------------------------------------------------------

void radio_init(void)
{
    memcpy(s_presets, DEFAULTS, sizeof s_presets);
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        radio_preset_t stored[RADIO_PRESETS];
        size_t len = sizeof stored;
        if (nvs_get_blob(h, "radio", stored, &len) == ESP_OK && len == sizeof stored) {
            bool ok = true;
            for (int i = 0; i < RADIO_PRESETS; i++) {
                stored[i].name[sizeof stored[i].name - 1] = '\0';
                stored[i].url[sizeof stored[i].url - 1] = '\0';
                if (stored[i].url[0] && (!url_ok(stored[i].url) || !name_ok(stored[i].name))) ok = false;
            }
            if (ok) memcpy(s_presets, stored, sizeof s_presets);
        }
        nvs_close(h);
    }
}

static esp_err_t get_handler(httpd_req_t *req)
{
    static char json[1800];
    int n = snprintf(json, sizeof json, "{\"playing\":%s,\"name\":\"%s\",\"status\":\"%s\",\"presets\":[",
                     s_running ? "true" : "false", s_running ? s_name : "", s_status);
    for (int i = 0; i < RADIO_PRESETS && n < (int)sizeof json - 260; i++) {
        n += snprintf(json + n, sizeof json - n, "%s{\"name\":\"%s\",\"url\":\"%s\"}", i ? "," : "", s_presets[i].name, s_presets[i].url);
    }
    snprintf(json + n, sizeof json - n, "]}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t save_handler(httpd_req_t *req)  // body lines: pN=name|url  (empty line value clears a slot)
{
    if (!web_authorized(req)) return web_deny(req);
    static char body[1500];
    if (req->content_len == 0 || req->content_len >= sizeof body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad body\n");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';
    radio_preset_t next[RADIO_PRESETS];
    memcpy(next, s_presets, sizeof next);
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int idx;
        int off = 0;
        if (sscanf(line, "p%d=%n", &idx, &off) < 0 || off == 0 || idx < 0 || idx >= RADIO_PRESETS) goto bad;
        char *val = line + off;
        if (*val == '\0') {
            memset(&next[idx], 0, sizeof next[idx]);
            continue;
        }
        char *bar = strchr(val, '|');
        if (!bar) goto bad;
        *bar = '\0';
        if (strlen(val) >= sizeof next[idx].name || strlen(bar + 1) >= sizeof next[idx].url) goto bad;
        if (!name_ok(val) || !url_ok(bar + 1)) goto bad;
        strlcpy(next[idx].name, val, sizeof next[idx].name);
        strlcpy(next[idx].url, bar + 1, sizeof next[idx].url);
    }
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) goto bad;
    bool ok = nvs_set_blob(h, "radio", next, sizeof next) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) goto bad;
    memcpy(s_presets, next, sizeof s_presets);
    return get_handler(req);
bad:
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "rejected: lines look like  p0=Station name|http://host/stream.mp3  (name max 23, url max 159, no spaces)\n");
}

static esp_err_t play_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[256], val[200];
    bool ok = false;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        if (httpd_query_key_value(query, "preset", val, sizeof val) == ESP_OK) {
            ok = radio_play_preset(atoi(val));
        } else if (httpd_query_key_value(query, "url", val, sizeof val) == ESP_OK) {
            // the url is passed percent-encoded by the page; decode the few characters that matter
            char url[200];
            size_t o = 0;
            for (size_t i = 0; val[i] && o + 1 < sizeof url; i++) {
                if (val[i] == '%' && val[i + 1] && val[i + 2]) {
                    char hex[3] = { val[i + 1], val[i + 2], 0 };
                    url[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    url[o++] = val[i];
                }
            }
            url[o] = '\0';
            ok = radio_play(url, "custom stream");
        }
    }
    if (!ok) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "cannot start: bad preset or URL (http:// or https://, no spaces)\n");
    }
    return get_handler(req);
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    if (s_running) media_abort();
    return get_handler(req);
}

void radio_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/radio",      .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/radio",      .method = HTTP_POST, .handler = save_handler },
        { .uri = "/radio/play", .method = HTTP_POST, .handler = play_handler },
        { .uri = "/radio/stop", .method = HTTP_POST, .handler = stop_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#endif  // CONFIG_AB_FEATURE_RADIO
