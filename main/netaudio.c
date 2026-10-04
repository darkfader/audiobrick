#include "netaudio.h"
#include "features.h"

#if CONFIG_AB_FEATURE_VBAN || CONFIG_AB_FEATURE_SCREAM

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "media.h"
#include "net.h"
#include "nvs.h"
#include "ota_http.h"
#include "safemode.h"

static const char *TAG = "netaudio";

#define LATENCY_HEADROOM_MS 180  // drop packets once the buffer is this far above the pre-buffer, so a live source never builds up delay
#define IDLE_END_MS      600    // no packets for this long: the stream is over
#define LOCKOUT_MS       1000   // after a stop, wait for the sender to go quiet before accepting it again
#define SILENCE_END_MS   4000   // a sender that keeps sending digital silence (Voicemeeter does) this long is treated as idle
#define SILENCE_LEVEL    16     // peak sample value that still counts as silence (16-bit scale)

static netaudio_cfg_t s_cfg;

// ---- settings -----------------------------------------------------------------------------

static bool ip_ok(const char *s)
{
    if (s[0] == '\0') return true;
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static bool valid(const netaudio_cfg_t *c)
{
    if (memchr(c->allow_ip, '\0', sizeof c->allow_ip) == NULL || !ip_ok(c->allow_ip)) return false;
    if (memchr(c->vban_name, '\0', sizeof c->vban_name) == NULL) return false;
    for (const char *p = c->vban_name; *p; p++) if (*p < 32 || *p > 126) return false;
    return true;
}

netaudio_cfg_t netaudio_get(void) { return s_cfg; }

bool netaudio_set(const netaudio_cfg_t *cfg)
{
    if (!valid(cfg)) return false;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "netaudio", cfg, sizeof *cfg) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        s_cfg = *cfg;
#if CONFIG_AB_FEATURE_SCREAM
        net_set_promiscuous(cfg->scream_on && cfg->scream_multicast);
#endif
    }
    return ok;
}

// ---- one receiver session ------------------------------------------------------------------

typedef struct {
    const char *label;        // for logs and the player label
    netaudio_stat_t stat;
    bool ours;                // we hold the main channel
    bool blocked;             // stopped by the user: ignore the sender until it goes quiet
    int64_t last_pkt_us;
    int64_t last_loud_us;     // last packet with real signal in it
    resampler_t rs;
    uint32_t rs_rate;
    int16_t stereo[2 * 1024]; // converted packet
    int16_t out[2 * 2048];    // resampled
} session_t;

#if CONFIG_AB_FEATURE_VBAN
static session_t s_vban = { .label = "VBAN" };
netaudio_stat_t netaudio_vban_stat(void) { return s_vban.stat; }
#else
netaudio_stat_t netaudio_vban_stat(void) { netaudio_stat_t z = { 0 }; return z; }
#endif
#if CONFIG_AB_FEATURE_SCREAM
static session_t s_scream = { .label = "Scream" };
netaudio_stat_t netaudio_scream_stat(void) { return s_scream.stat; }
#else
netaudio_stat_t netaudio_scream_stat(void) { netaudio_stat_t z = { 0 }; return z; }
#endif

static void end_session(session_t *s)
{
    if (s->ours) {
        media_finish();  // let the buffer drain
        s->ours = false;
    }
    s->stat.active = false;
}

// Called for every received audio packet after it has been converted to 16-bit stereo frames.
static void deliver(session_t *s, const char *label, const struct sockaddr_in *from, int16_t *frames, size_t n, uint32_t rate)
{
    if (rate < 8000 || rate > 768000 || n == 0) return;
    int64_t now = esp_timer_get_time();
    s->last_pkt_us = now;
    s->stat.packets++;
    // Always-on senders such as Voicemeeter's VBAN stream keep sending zeros when nothing plays. Without this the
    // stream would hold the channel for ever and the amp could never go idle, mute or power down.
    int peak = 0;
    for (size_t i = 0; i < 2 * n; i++) {
        int v = frames[i] < 0 ? -frames[i] : frames[i];
        if (v > peak) peak = v;
    }
    if (peak > SILENCE_LEVEL) s->last_loud_us = now;
    bool silent_long = now - s->last_loud_us > (int64_t)SILENCE_END_MS * 1000;
    if (silent_long) {
        if (s->ours) end_session(s);
        s->stat.active = false;
        return;  // do not start (or keep) a session for silence
    }
    if (s->blocked) return;
    if (s->ours && media_aborted()) {  // the user pressed stop: stay quiet until the sender stops
        s->ours = false;
        s->blocked = true;
        s->stat.active = false;
        return;
    }
    if (!s->ours) {
        if (media_active()) { s->stat.dropped++; return; }  // something else is playing
        if (!media_begin(MEDIA_STREAM, label)) { s->stat.dropped++; return; }
        s->ours = true;
        s->rs_rate = 0;
        s->stat.active = true;
        s->stat.rate = rate;
        inet_ntop(AF_INET, &from->sin_addr, s->stat.source, sizeof s->stat.source);
        ESP_LOGI(TAG, "%s stream from %s at %u Hz", s->label, s->stat.source, (unsigned)rate);
    }
    if (media_buffer_ms() > media_prebuffer_ms() + LATENCY_HEADROOM_MS) { s->stat.dropped++; return; }

    if (rate == 48000) {
        media_write_nb_slot(SLOT_MAIN, frames, n);
        return;
    }
    if (rate != s->rs_rate) {
        resampler_init(&s->rs, rate);
        s->rs_rate = rate;
    }
    for (size_t done = 0; done < n;) {
        size_t slice = n - done > 256 ? 256 : n - done;
        size_t produced = resampler_process(&s->rs, frames + 2 * done, slice, s->out, 2048);
        if (produced) media_write_nb_slot(SLOT_MAIN, s->out, produced);
        done += slice;
    }
}

// Periodic housekeeping: end a session when the sender goes quiet, release the lockout.
static void tick(session_t *s)
{
    int64_t quiet_ms = (esp_timer_get_time() - s->last_pkt_us) / 1000;
    if (s->ours && quiet_ms > IDLE_END_MS) end_session(s);
    if (s->blocked && quiet_ms > LOCKOUT_MS) s->blocked = false;
    if (!s->ours && quiet_ms > IDLE_END_MS) s->stat.active = false;
}

static bool sender_allowed(const struct sockaddr_in *from)
{
    if (s_cfg.allow_ip[0] == '\0') return true;
    char ip[16];
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
    return strcmp(ip, s_cfg.allow_ip) == 0;
}

static int open_udp(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return -1;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(sock);
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return sock;
}

// ---- VBAN ------------------------------------------------------------------------------------

#if CONFIG_AB_FEATURE_VBAN
static const uint32_t VBAN_RATES[21] = { 6000, 12000, 24000, 48000, 96000, 192000, 384000, 8000, 16000, 32000, 64000,
                                         128000, 256000, 512000, 11025, 22050, 44100, 88200, 176400, 352800, 705600 };

#endif  // VBAN rates

static int16_t to_s16(const uint8_t *p, int fmt)
{
    switch (fmt) {
    case 0: return (int16_t)(((int)p[0] - 128) << 8);                         // unsigned 8 bit
    case 1: return (int16_t)(p[0] | (p[1] << 8));                            // 16 bit
    case 2: return (int16_t)(p[1] | (p[2] << 8));                            // 24 bit: keep the top 16
    case 3: return (int16_t)(p[2] | (p[3] << 8));                            // 32 bit: keep the top 16
    case 4: {                                                                // 32 bit float
        float f;
        memcpy(&f, p, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int16_t)(f * 32767.0f);
    }
    default: return 0;
    }
}

#if CONFIG_AB_FEATURE_VBAN
static const int BYTES_PER_SAMPLE[5] = { 1, 2, 3, 4, 4 };

static void vban_task(void *arg)
{
    static uint8_t pkt[2048];
    int sock = -1;
    while (true) {
        if (!s_cfg.vban_on) {
            if (sock >= 0) { close(sock); sock = -1; end_session(&s_vban); }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!net_has_ip()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }  // the network stack must be running first
        if (sock < 0 && (sock = open_udp(VBAN_PORT)) < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(sock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &flen);
        tick(&s_vban);
        if (n < 28 || memcmp(pkt, "VBAN", 4) != 0) continue;
        if (!sender_allowed(&from)) { s_vban.stat.dropped++; continue; }
        if ((pkt[4] & 0xE0) != 0x00) continue;            // not audio (serial, text, service)
        if ((pkt[7] & 0xF0) != 0x00) continue;            // not PCM
        int sr = pkt[4] & 0x1F, fmt = pkt[7] & 0x07;
        if (sr > 20 || fmt > 4) continue;
        int nbs = pkt[5] + 1, nbc = pkt[6] + 1;
        char name[17];
        memcpy(name, pkt + 8, 16);
        name[16] = '\0';
        if (s_cfg.vban_name[0] && strcmp(name, s_cfg.vban_name) != 0) continue;
        int bps = BYTES_PER_SAMPLE[fmt];
        if (28 + nbs * nbc * bps > n || nbs > 1024) continue;
        strlcpy(s_vban.stat.name, name, sizeof s_vban.stat.name);
        s_vban.stat.channels = nbc;
        const uint8_t *d = pkt + 28;
        for (int i = 0; i < nbs; i++) {
            const uint8_t *fr = d + (size_t)i * nbc * bps;
            s_vban.stereo[2 * i] = to_s16(fr, fmt);
            s_vban.stereo[2 * i + 1] = nbc > 1 ? to_s16(fr + bps, fmt) : s_vban.stereo[2 * i];
        }
        char label[24];
        snprintf(label, sizeof label, "VBAN %s", name);
        deliver(&s_vban, label, &from, s_vban.stereo, nbs, VBAN_RATES[sr]);
    }
}

#endif  // CONFIG_AB_FEATURE_VBAN

// ---- Scream ----------------------------------------------------------------------------------
#if CONFIG_AB_FEATURE_SCREAM

static void scream_task(void *arg)
{
    static uint8_t pkt[1500];
    int sock = -1;
    bool joined = false;
    while (true) {
        if (!s_cfg.scream_on) {
            if (sock >= 0) { close(sock); sock = -1; joined = false; end_session(&s_scream); }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!net_has_ip()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }  // the network stack must be running first
        if (sock < 0 && (sock = open_udp(SCREAM_PORT)) < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        if (s_cfg.scream_multicast && !joined && net_has_ip()) {
            struct ip_mreq mreq = { .imr_multiaddr.s_addr = inet_addr("239.255.77.77"), .imr_interface.s_addr = htonl(INADDR_ANY) };
            joined = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) == 0;
            ESP_LOGI(TAG, "Scream multicast %s", joined ? "joined" : "join failed");
        }
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(sock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &flen);
        tick(&s_scream);
        if (n < 6) continue;
        if (!sender_allowed(&from)) { s_scream.stat.dropped++; continue; }
        uint32_t rate = ((pkt[0] & 0x80) ? 44100 : 48000) * ((pkt[0] & 0x7F) ? (pkt[0] & 0x7F) : 1);
        int bits = pkt[1], ch = pkt[2];
        if ((bits != 16 && bits != 24 && bits != 32) || ch < 1 || ch > 8) continue;
        int bps = bits / 8;
        int frames = (n - 5) / (bps * ch);
        if (frames <= 0 || frames > 1024) continue;
        s_scream.stat.channels = ch;
        const uint8_t *d = pkt + 5;
        int fmt = bits == 16 ? 1 : (bits == 24 ? 2 : 3);
        for (int i = 0; i < frames; i++) {
            const uint8_t *fr = d + (size_t)i * ch * bps;
            s_scream.stereo[2 * i] = to_s16(fr, fmt);
            s_scream.stereo[2 * i + 1] = ch > 1 ? to_s16(fr + bps, fmt) : s_scream.stereo[2 * i];
        }
        deliver(&s_scream, "Scream", &from, s_scream.stereo, frames, rate);
    }
}

#endif  // CONFIG_AB_FEATURE_SCREAM

void netaudio_init(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.scream_multicast = true;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        netaudio_cfg_t stored;
        size_t len = sizeof stored;
        if (nvs_get_blob(h, "netaudio", &stored, &len) == ESP_OK && len == sizeof stored && valid(&stored)) s_cfg = stored;
        nvs_close(h);
    }
    if (!AB_HAS_VBAN) s_cfg.vban_on = false;      // a receiver that is not built can never be on
    if (!AB_HAS_SCREAM) s_cfg.scream_on = false;
    if (safemode_active()) {  // keep the saved settings, but do not act on them this boot
        s_cfg.vban_on = false;
        s_cfg.scream_on = false;
    }
#if CONFIG_AB_FEATURE_VBAN
    xTaskCreate(vban_task, "vban", 4096, NULL, 4, NULL);
#endif
#if CONFIG_AB_FEATURE_SCREAM
    xTaskCreate(scream_task, "scream", 4096, NULL, 4, NULL);
#endif
}

// ---- HTTP --------------------------------------------------------------------------------------

static void stat_json(char *dst, size_t n, const netaudio_stat_t *s)
{
    snprintf(dst, n, "{\"active\":%s,\"packets\":%u,\"dropped\":%u,\"rate\":%u,\"channels\":%d,\"source\":\"%s\",\"name\":\"%s\"}",
             s->active ? "true" : "false", (unsigned)s->packets, (unsigned)s->dropped, (unsigned)s->rate, s->channels,
             s->source, s->name);
}

static esp_err_t get_handler(httpd_req_t *req)
{
    char a[200], b[200], json[640];
    netaudio_stat_t v = netaudio_vban_stat(), sc = netaudio_scream_stat();
    stat_json(a, sizeof a, &v);
    stat_json(b, sizeof b, &sc);
    snprintf(json, sizeof json,
             "{\"vban_on\":%s,\"scream_on\":%s,\"scream_multicast\":%s,\"allow_ip\":\"%s\",\"vban_name\":\"%s\","
             "\"vban_port\":%d,\"scream_port\":%d,\"vban_supported\":%s,\"scream_supported\":%s,\"vban\":%s,\"scream\":%s}\n",
             s_cfg.vban_on ? "true" : "false", s_cfg.scream_on ? "true" : "false", s_cfg.scream_multicast ? "true" : "false",
             s_cfg.allow_ip, s_cfg.vban_name, VBAN_PORT, SCREAM_PORT, AB_HAS_VBAN ? "true" : "false",
             AB_HAS_SCREAM ? "true" : "false", a, b);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[256];
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
    netaudio_cfg_t c = s_cfg;
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int v;
        char text[20];
        if (sscanf(line, "vban=%d", &v) == 1) c.vban_on = v != 0;
        else if (sscanf(line, "scream=%d", &v) == 1) c.scream_on = v != 0;
        else if (sscanf(line, "scream_multicast=%d", &v) == 1) c.scream_multicast = v != 0;
        else if (strncmp(line, "allow_ip=", 9) == 0) strlcpy(c.allow_ip, line + 9, sizeof c.allow_ip);
        else if (strncmp(line, "vban_name=", 10) == 0) strlcpy(c.vban_name, line + 10, sizeof c.vban_name);
        else if (sscanf(line, "%19s", text) != 1) continue;
        else {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "cannot parse a line\n");
        }
    }
    if ((c.vban_on && !AB_HAS_VBAN) || (c.scream_on && !AB_HAS_SCREAM)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "that receiver is not built into this firmware (see docs/windows-setup.md; enable it in menuconfig, Audio Brick features)\n");
    }
    if (!netaudio_set(&c)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "rejected: allow_ip must be a dotted IPv4 address (or empty), vban_name printable, max 16 characters\n");
    }
    return get_handler(req);
}

void netaudio_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/netaudio", .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/netaudio", .method = HTTP_POST, .handler = post_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#endif  // CONFIG_AB_FEATURE_VBAN || CONFIG_AB_FEATURE_SCREAM
