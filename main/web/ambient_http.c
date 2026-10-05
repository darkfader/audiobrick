// HTTP endpoints for the ambient scene and the player transport controls.
//   GET  /ambient                 scene settings and what is playing (no login)
//   POST /ambient                 body: "enabled=1" and "rN=on,type,clip,min_s,max_s,gain_db" lines (login)
//                                 type: 0 event (random interval), 1 background (loops)
//   POST /ambient/enable?on=1     switch the whole scene on or off (login)
//   GET  /player                  {"state","clip","index","count","autonext"} (no login)
//   POST /player/play|pause|toggle|next|prev|stop   (login)
//   POST /player/autonext?on=1    advance by itself when a clip ends (login)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "ambient.h"
#include "bluetooth.h"
#include "dac.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "netaudio.h"
#include "ota_http.h"
#include "player.h"
#include "radio.h"
#include "synth.h"
#include "wifi.h"

#if CONFIG_AB_FEATURE_AMBIENT
static esp_err_t ambient_get_handler(httpd_req_t *req)
{
    ambient_config_t c = ambient_get();
    static char json[1800];
    int n = snprintf(json, sizeof json, "{\"enabled\":%s,\"running\":%s,\"bg\":\"%s\",\"event\":\"%s\",\"rules\":[",
                     c.enabled ? "true" : "false", ambient_running() ? "true" : "false",
                     media_active_slot(SLOT_BG) ? media_label_slot(SLOT_BG) : "",
                     media_active_slot(SLOT_EVENT) ? media_label_slot(SLOT_EVENT) : "");
    for (int i = 0; i < AMBIENT_MAX_RULES && n < (int)sizeof json - 200; i++) {
        const ambient_rule_t *r = &c.rule[i];
        n += snprintf(json + n, sizeof json - n,
                      "%s{\"on\":%d,\"type\":%d,\"clip\":\"%s\",\"min\":%u,\"max\":%u,\"gain\":%.1f,\"next_in\":%d}",
                      i ? "," : "", r->on ? 1 : 0, r->type, r->clip, (unsigned)r->min_s, (unsigned)r->max_s,
                      r->gain_db, ambient_next_in_s(i));
    }
    snprintf(json + n, sizeof json - n, "]}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t ambient_post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    static char body[1400];
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

    ambient_config_t c = ambient_get();
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int on, type, idx;
        char clip[40];
        unsigned mn, mx;
        float g;
        if (sscanf(line, "enabled=%d", &on) == 1) {
            c.enabled = on != 0;
        } else if (sscanf(line, "r%d=%d,%d,%39[^,],%u,%u,%f", &idx, &on, &type, clip, &mn, &mx, &g) == 7 && idx >= 0 &&
                   idx < AMBIENT_MAX_RULES && strlen(clip) < sizeof c.rule[idx].clip) {
            ambient_rule_t *r = &c.rule[idx];
            r->on = on != 0;
            r->type = (uint8_t)type;
            strlcpy(r->clip, clip, sizeof r->clip);
            r->min_s = mn;
            r->max_s = mx;
            r->gain_db = g;
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "cannot parse a line\n");
        }
    }
    if (!ambient_set(&c)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "rejected: check clip names, intervals (5 s to 24 h, min <= max) and gain (-30..0 dB)\n");
    }
    return ambient_get_handler(req);
}

static esp_err_t ambient_enable_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[32], val[8];
    ambient_config_t c = ambient_get();
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "on", val, sizeof val) == ESP_OK) {
        c.enabled = atoi(val) != 0;
    }
    ambient_set(&c);
    return ambient_get_handler(req);
}
#endif  // CONFIG_AB_FEATURE_AMBIENT

#if CONFIG_AB_FEATURE_CLIPS  // AB_GATE_PLAYER: the transport endpoints
static const char *state_name(player_state_t s)
{
    return s == PLAYER_PLAYING ? "playing" : (s == PLAYER_PAUSED ? "paused" : "idle");
}

static esp_err_t player_get_handler(httpd_req_t *req)
{
    char json[220];
    snprintf(json, sizeof json, "{\"state\":\"%s\",\"clip\":\"%s\",\"index\":%d,\"count\":%d,\"autonext\":%s}\n",
             state_name(player_state()), player_current(), player_index(), player_count(),
             player_autonext() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t transport(httpd_req_t *req, bool (*fn)(void))
{
    if (!web_authorized(req)) return web_deny(req);
    if (!fn()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "cannot do that now (nothing to play, or a stream is active)\n");
    }
    vTaskDelay(pdMS_TO_TICKS(60));  // let the state settle before reporting it
    return player_get_handler(req);
}

static bool do_toggle(void) { return player_state() == PLAYER_PLAYING ? player_pause() : player_play(); }
static esp_err_t play_h(httpd_req_t *r)   { return transport(r, player_play); }
static esp_err_t pause_h(httpd_req_t *r)  { return transport(r, player_pause); }
static esp_err_t toggle_h(httpd_req_t *r) { return transport(r, do_toggle); }
static esp_err_t next_h(httpd_req_t *r)   { return transport(r, player_next); }
static esp_err_t prev_h(httpd_req_t *r)   { return transport(r, player_prev); }
static esp_err_t stop_h(httpd_req_t *r)   { return transport(r, player_stop); }

static esp_err_t autonext_h(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "on", val, sizeof val) == ESP_OK) {
        player_set_autonext(atoi(val) != 0);
    }
    return player_get_handler(req);
}

#endif  // CONFIG_AB_FEATURE_CLIPS

void ambient_http_register(httpd_handle_t server)
{
#if CONFIG_AB_FEATURE_AMBIENT || CONFIG_AB_FEATURE_CLIPS
    const httpd_uri_t uris[] = {
#if CONFIG_AB_FEATURE_AMBIENT
        { .uri = "/ambient",        .method = HTTP_GET,  .handler = ambient_get_handler },
        { .uri = "/ambient",        .method = HTTP_POST, .handler = ambient_post_handler },
        { .uri = "/ambient/enable", .method = HTTP_POST, .handler = ambient_enable_handler },
#endif
#if CONFIG_AB_FEATURE_CLIPS
        { .uri = "/player",         .method = HTTP_GET,  .handler = player_get_handler },
        { .uri = "/player/play",    .method = HTTP_POST, .handler = play_h },
        { .uri = "/player/pause",   .method = HTTP_POST, .handler = pause_h },
        { .uri = "/player/toggle",  .method = HTTP_POST, .handler = toggle_h },
        { .uri = "/player/next",    .method = HTTP_POST, .handler = next_h },
        { .uri = "/player/prev",    .method = HTTP_POST, .handler = prev_h },
        { .uri = "/player/stop",    .method = HTTP_POST, .handler = stop_h },
        { .uri = "/player/autonext", .method = HTTP_POST, .handler = autonext_h },
#endif
    };
#endif
#if CONFIG_AB_FEATURE_AMBIENT || CONFIG_AB_FEATURE_CLIPS
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
#endif
    dac_http_register(server);
#if CONFIG_AB_FEATURE_BLUETOOTH
    bluetooth_http_register(server);
#endif
#if CONFIG_AB_FEATURE_WIFI
    wifi_client_http_register(server);
#endif
#if CONFIG_AB_FEATURE_VBAN || CONFIG_AB_FEATURE_SCREAM
    netaudio_http_register(server);
#endif
#if CONFIG_AB_FEATURE_RADIO
    radio_http_register(server);
#endif
#if CONFIG_AB_FEATURE_SYNTH
    synth_http_register(server);
#endif
}
