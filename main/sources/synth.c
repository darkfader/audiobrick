#include "synth.h"

#if CONFIG_AB_FEATURE_SYNTH


#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net.h"
#include "nvs.h"
#include "ota_http.h"
#include "safemode.h"
#include "speaker_limits.h"

static const char *TAG = "synth";

#define VOICES 8

typedef enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } env_state_t;

typedef struct {
    env_state_t env;
    float level;        // envelope level 0..1
    float phase;        // 0..1
    float inc;          // phase increment per sample
    float vel;          // 0..1
    int note;           // MIDI note, -1 for the frequency-controlled voice
    uint32_t age;       // for voice stealing
    int64_t off_at_us;  // automatic release time, 0 = none
} voice_t;

typedef enum { EV_ON, EV_OFF, EV_ALLOFF, EV_FREQ } ev_type_t;
typedef struct {
    ev_type_t type;
    int note;
    float vel, freq;
    uint32_t dur_ms;
} event_t;

static synth_cfg_t s_cfg;
static voice_t s_voice[VOICES];
static QueueHandle_t s_queue;
static uint32_t s_age;
static volatile bool s_sounding;
static float s_lp_l, s_lp_r;
static volatile uint32_t s_packets, s_messages;

static const synth_cfg_t DEFAULTS = {
    .enabled = false, .allow_ip = "", .wave = 0, .a = 0.01f, .d = 0.10f, .s = 0.80f, .r = 0.20f,
    .cutoff = 12000.0f, .volume = 0.5f, .max_note_s = 30.0f,
};

synth_cfg_t synth_get(void) { return s_cfg; }
bool synth_busy(void) { return s_sounding || (s_queue && uxQueueMessagesWaiting(s_queue) > 0); }

static bool valid(const synth_cfg_t *c)
{
    if (memchr(c->allow_ip, '\0', sizeof c->allow_ip) == NULL) return false;
    if (c->allow_ip[0]) {
        struct in_addr a;
        if (inet_pton(AF_INET, c->allow_ip, &a) != 1) return false;
    }
    return c->wave <= 3 && c->a >= 0.001f && c->a <= 10 && c->d >= 0.001f && c->d <= 10 && c->s >= 0 && c->s <= 1 &&
           c->r >= 0.001f && c->r <= 10 && c->cutoff >= 50 && c->cutoff <= 20000 && c->volume >= 0 && c->volume <= 1 &&
           c->max_note_s >= 1 && c->max_note_s <= 600;
}

bool synth_set(const synth_cfg_t *cfg)
{
    if (!valid(cfg)) return false;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "synth", cfg, sizeof *cfg) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) s_cfg = *cfg;
    return ok;
}

static void post(const event_t *e)
{
    if (s_queue) xQueueSend(s_queue, e, 0);
}

void synth_note(int note, int velocity, uint32_t dur_ms)
{
    if (note < 0 || note > 127) return;
    event_t e = { .type = velocity > 0 ? EV_ON : EV_OFF, .note = note, .vel = velocity / 127.0f, .dur_ms = dur_ms };
    post(&e);
}

// ---- audio ---------------------------------------------------------------------------------------

static float osc(uint8_t wave, float ph, float inc)
{
    switch (wave) {
    case 1:  // triangle
        return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    case 2: {  // saw, band-limited at the jump (PolyBLEP)
        float y = 2.0f * ph - 1.0f;
        if (ph < inc) { float t = ph / inc; y -= t + t - t * t - 1.0f; }
        else if (ph > 1.0f - inc) { float t = (ph - 1.0f) / inc; y -= t * t + t + t + 1.0f; }
        return y;
    }
    case 3: {  // square
        float y = ph < 0.5f ? 1.0f : -1.0f;
        if (ph < inc) { float t = ph / inc; y += t + t - t * t - 1.0f; }
        else if (ph > 1.0f - inc) { float t = (ph - 1.0f) / inc; y += t * t + t + t + 1.0f; }
        float p2 = ph + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        if (p2 < inc) { float t = p2 / inc; y -= t + t - t * t - 1.0f; }
        else if (p2 > 1.0f - inc) { float t = (p2 - 1.0f) / inc; y -= t * t + t + t + 1.0f; }
        return y;
    }
    default:
        return sinf(2.0f * (float)M_PI * ph);
    }
}

static voice_t *alloc_voice(int note)
{
    voice_t *best = NULL;
    for (int i = 0; i < VOICES; i++) {  // retrigger the same note
        if (s_voice[i].env != ENV_IDLE && s_voice[i].note == note) return &s_voice[i];
    }
    for (int i = 0; i < VOICES; i++) if (s_voice[i].env == ENV_IDLE) return &s_voice[i];
    for (int i = 0; i < VOICES; i++) {  // steal: prefer releasing voices, then the oldest
        voice_t *v = &s_voice[i];
        if (!best || (v->env == ENV_RELEASE && best->env != ENV_RELEASE) ||
            ((v->env == ENV_RELEASE) == (best->env == ENV_RELEASE) && v->age < best->age)) best = v;
    }
    return best;
}

static void start_voice(int note, float freq, float vel, uint32_t dur_ms)
{
    float lowest = limits_get().min_hz;  // speaker protection: nothing below the profile's lowest frequency
    if (freq < lowest || freq > 20000.0f) return;
    voice_t *v = alloc_voice(note);
    v->note = note;
    v->inc = freq / SAMPLE_RATE;
    v->vel = vel;
    v->env = ENV_ATTACK;
    v->age = ++s_age;
    int64_t limit_us = (int64_t)(s_cfg.max_note_s * 1e6f);
    int64_t want_us = dur_ms ? (int64_t)dur_ms * 1000 : limit_us;
    if (want_us > limit_us) want_us = limit_us;
    v->off_at_us = esp_timer_get_time() + want_us;
}

static void release_note(int note)
{
    for (int i = 0; i < VOICES; i++) {
        if (s_voice[i].env != ENV_IDLE && s_voice[i].env != ENV_RELEASE && s_voice[i].note == note) s_voice[i].env = ENV_RELEASE;
    }
}

static void handle_event(const event_t *e)
{
    switch (e->type) {
    case EV_ON:     start_voice(e->note, 440.0f * powf(2.0f, (e->note - 69) / 12.0f), e->vel, e->dur_ms); break;
    case EV_OFF:    release_note(e->note); break;
    case EV_FREQ:
        if (e->vel > 0) start_voice(-1, e->freq, e->vel, e->dur_ms);
        else release_note(-1);
        break;
    case EV_ALLOFF:
        for (int i = 0; i < VOICES; i++) if (s_voice[i].env != ENV_IDLE) s_voice[i].env = ENV_RELEASE;
        break;
    }
}

static inline float soft_clip(float x)
{
    float a = fabsf(x);
    if (a <= 0.7f) return x;
    float y = 0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f);
    return x < 0 ? -y : y;
}

bool synth_render(float *left, float *right, int frames)
{
    event_t e;
    while (s_queue && xQueueReceive(s_queue, &e, 0) == pdTRUE) handle_event(&e);

    int64_t now = esp_timer_get_time();
    bool any = false;
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->env != ENV_IDLE) any = true;
        if (v->env != ENV_IDLE && v->env != ENV_RELEASE && v->off_at_us && now >= v->off_at_us) v->env = ENV_RELEASE;
    }
    if (!any) {
        s_sounding = false;
        s_lp_l = s_lp_r = 0.0f;
        return false;
    }

    const synth_cfg_t c = s_cfg;
    const float att = 1.0f / (c.a * SAMPLE_RATE), dec = (1.0f - c.s) / (c.d * SAMPLE_RATE), rel = 1.0f / (c.r * SAMPLE_RATE);
    const float gain = c.volume * 0.35f;
    const float lp = 1.0f - expf(-2.0f * (float)M_PI * c.cutoff / SAMPLE_RATE);

    for (int n = 0; n < frames; n++) {
        float mix = 0.0f;
        for (int i = 0; i < VOICES; i++) {
            voice_t *v = &s_voice[i];
            if (v->env == ENV_IDLE) continue;
            switch (v->env) {
            case ENV_ATTACK:  v->level += att; if (v->level >= 1.0f) { v->level = 1.0f; v->env = ENV_DECAY; } break;
            case ENV_DECAY:   v->level -= dec; if (v->level <= c.s) { v->level = c.s; v->env = ENV_SUSTAIN; } break;
            case ENV_RELEASE: v->level -= rel * (v->level > 0.2f ? 1.0f : 0.5f); if (v->level <= 0.0005f) { v->level = 0; v->env = ENV_IDLE; } break;
            default: break;
            }
            if (v->env == ENV_IDLE) continue;
            mix += osc(c.wave, v->phase, v->inc) * v->level * v->vel;
            v->phase += v->inc;
            if (v->phase >= 1.0f) v->phase -= 1.0f;
        }
        float x = mix * gain;
        s_lp_l += lp * (x - s_lp_l);
        float y = soft_clip(s_lp_l) * 32767.0f;
        left[n] = y;
        right[n] = y;
    }
    s_sounding = true;
    return true;
}

// ---- OSC ------------------------------------------------------------------------------------------

static uint32_t rd_u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static void on_message(const uint8_t *m, int len)
{
    const uint8_t *end = m + len;
    const char *addr = (const char *)m;
    size_t al = strnlen(addr, len);
    if (al == (size_t)len) return;
    const uint8_t *p = m + ((al + 4) & ~3u);
    if (p >= end) return;
    const char *tags = (const char *)p;
    if (tags[0] != ',') return;
    size_t tl = strnlen(tags, end - p);
    p += (tl + 4) & ~3u;

    float num[6] = { 0 };
    int nn = 0;
    char str[16] = "";
    for (const char *t = tags + 1; *t && p <= end; t++) {
        switch (*t) {
        case 'i': if (p + 4 > end) return; if (nn < 6) num[nn++] = (float)(int32_t)rd_u32(p); p += 4; break;
        case 'f': { if (p + 4 > end) return; uint32_t u = rd_u32(p); float f; memcpy(&f, &u, 4); if (nn < 6) num[nn++] = f; p += 4; break; }
        case 'd': { if (p + 8 > end) return; uint64_t u = ((uint64_t)rd_u32(p) << 32) | rd_u32(p + 4); double d; memcpy(&d, &u, 8); if (nn < 6) num[nn++] = (float)d; p += 8; break; }
        case 'h': case 't': p += 8; break;
        case 's': { size_t sl = strnlen((const char *)p, end - p); strlcpy(str, (const char *)p, sizeof str); p += (sl + 4) & ~3u; break; }
        case 'T': if (nn < 6) num[nn++] = 1; break;
        case 'F': if (nn < 6) num[nn++] = 0; break;
        default: break;
        }
    }
    s_messages++;

    if (strcmp(addr, "/synth/note") == 0 && nn >= 1) {
        synth_note((int)num[0], nn >= 2 ? (int)num[1] : 100, 0);
    } else if (strcmp(addr, "/synth/noteoff") == 0 && nn >= 1) {
        event_t e = { .type = EV_OFF, .note = (int)num[0] };
        post(&e);
    } else if (strcmp(addr, "/synth/alloff") == 0) {
        event_t e = { .type = EV_ALLOFF };
        post(&e);
    } else if (strcmp(addr, "/synth/freq") == 0 && nn >= 1) {
        event_t e = { .type = EV_FREQ, .freq = num[0], .vel = (nn >= 2 ? num[1] : 100.0f) / 127.0f };
        post(&e);
    } else if (strcmp(addr, "/synth/gate") == 0 && nn >= 1) {
        if (num[0] <= 0) {  // gate on needs a frequency: use /synth/freq; gate 0 releases the monophonic voice
            event_t e = { .type = EV_OFF, .note = -1 };
            post(&e);
        }
    } else if (strcmp(addr, "/synth/wave") == 0) {
        synth_cfg_t c = s_cfg;
        if (str[0]) c.wave = !strcasecmp(str, "tri") || !strcasecmp(str, "triangle") ? 1 : !strcasecmp(str, "saw") ? 2 : !strcasecmp(str, "square") ? 3 : 0;
        else if (nn >= 1) c.wave = (uint8_t)num[0];
        if (valid(&c)) s_cfg.wave = c.wave;
    } else if (strcmp(addr, "/synth/adsr") == 0 && nn >= 4) {
        synth_cfg_t c = s_cfg;
        c.a = num[0]; c.d = num[1]; c.s = num[2]; c.r = num[3];
        if (valid(&c)) { s_cfg.a = c.a; s_cfg.d = c.d; s_cfg.s = c.s; s_cfg.r = c.r; }
    } else if (strcmp(addr, "/synth/cutoff") == 0 && nn >= 1) {
        if (num[0] >= 50 && num[0] <= 20000) s_cfg.cutoff = num[0];
    } else if (strcmp(addr, "/synth/volume") == 0 && nn >= 1) {
        if (num[0] >= 0 && num[0] <= 1) s_cfg.volume = num[0];
    }
    // Live changes through OSC are not stored; the page's Save button stores the settings.
}

static void on_packet(const uint8_t *p, int len, int depth)
{
    if (len < 8 || depth > 3) return;
    if (memcmp(p, "#bundle", 8) == 0) {
        int off = 16;
        while (off + 4 <= len) {
            int sz = (int)rd_u32(p + off);
            off += 4;
            if (sz <= 0 || off + sz > len) return;
            on_packet(p + off, sz, depth + 1);
            off += (sz + 3) & ~3;
        }
    } else if (p[0] == '/') {
        on_message(p, len);
    }
}

static void osc_task(void *arg)
{
    static uint8_t pkt[1024];
    int sock = -1;
    while (true) {
        if (!s_cfg.enabled || safemode_active() || !net_has_ip()) {
            if (sock >= 0) { close(sock); sock = -1; }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(OSC_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
            if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof a) != 0) {
                if (sock >= 0) close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ESP_LOGI(TAG, "OSC listening on UDP %d", OSC_PORT);
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        if (s_cfg.allow_ip[0]) {
            char ip[16];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            if (strcmp(ip, s_cfg.allow_ip) != 0) continue;
        }
        s_packets++;
        on_packet(pkt, n, 0);
    }
}

void synth_init(void)
{
    s_cfg = DEFAULTS;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        synth_cfg_t stored;
        size_t len = sizeof stored;
        if (nvs_get_blob(h, "synth", &stored, &len) == ESP_OK && len == sizeof stored && valid(&stored)) s_cfg = stored;
        nvs_close(h);
    }
    s_queue = xQueueCreate(32, sizeof(event_t));
    xTaskCreate(osc_task, "osc", 4096, NULL, 4, NULL);
}

// ---- HTTP -----------------------------------------------------------------------------------------

static const char *WAVES[] = { "sine", "triangle", "saw", "square" };

static esp_err_t get_handler(httpd_req_t *req)
{
    synth_cfg_t c = s_cfg;
    int active = 0;
    for (int i = 0; i < VOICES; i++) if (s_voice[i].env != ENV_IDLE) active++;
    char json[420];
    snprintf(json, sizeof json,
             "{\"enabled\":%s,\"port\":%d,\"allow_ip\":\"%s\",\"wave\":%d,\"wave_name\":\"%s\",\"a\":%.3f,\"d\":%.3f,\"s\":%.2f,\"r\":%.3f,"
             "\"cutoff\":%.0f,\"volume\":%.2f,\"max_note_s\":%.0f,\"voices_active\":%d,\"packets\":%u,\"messages\":%u}\n",
             c.enabled ? "true" : "false", OSC_PORT, c.allow_ip, c.wave, WAVES[c.wave], c.a, c.d, c.s, c.r, c.cutoff, c.volume,
             c.max_note_s, active, (unsigned)s_packets, (unsigned)s_messages);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[300];
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
    synth_cfg_t c = s_cfg;
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int v;
        float f;
        if (sscanf(line, "enabled=%d", &v) == 1) c.enabled = v != 0;
        else if (strncmp(line, "allow_ip=", 9) == 0) strlcpy(c.allow_ip, line + 9, sizeof c.allow_ip);
        else if (sscanf(line, "wave=%d", &v) == 1) c.wave = (uint8_t)v;
        else if (sscanf(line, "a=%f", &f) == 1) c.a = f;
        else if (sscanf(line, "d=%f", &f) == 1) c.d = f;
        else if (sscanf(line, "s=%f", &f) == 1) c.s = f;
        else if (sscanf(line, "r=%f", &f) == 1) c.r = f;
        else if (sscanf(line, "cutoff=%f", &f) == 1) c.cutoff = f;
        else if (sscanf(line, "volume=%f", &f) == 1) c.volume = f;
        else if (sscanf(line, "max_note_s=%f", &f) == 1) c.max_note_s = f;
        else {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "cannot parse a line\n");
        }
    }
    if (!synth_set(&c)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "rejected: value out of range (times 0.001-10 s, sustain and volume 0-1, cutoff 50-20000, allow_ip a dotted address or empty)\n");
    }
    return get_handler(req);
}

// Test key from the web page: POST /synth/note?note=60&vel=100&dur=600
static esp_err_t note_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char q[96], v[16];
    int note = 60, vel = 100, dur = 600;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "note", v, sizeof v) == ESP_OK) note = atoi(v);
        if (httpd_query_key_value(q, "vel", v, sizeof v) == ESP_OK) vel = atoi(v);
        if (httpd_query_key_value(q, "dur", v, sizeof v) == ESP_OK) dur = atoi(v);
    }
    if (dur < 20) dur = 20;
    if (dur > 5000) dur = 5000;
    synth_note(note, vel, (uint32_t)dur);
    return httpd_resp_sendstr(req, "ok\n");
}

void synth_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/synth",      .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/synth",      .method = HTTP_POST, .handler = post_handler },
        { .uri = "/synth/note", .method = HTTP_POST, .handler = note_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#endif  // CONFIG_AB_FEATURE_SYNTH
