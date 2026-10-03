// HTTP endpoints for the EQ.
//   GET  /eq                 current settings, preamp and a sampled response (no login)
//   POST /eq                 body: text lines "enabled=1" and "b0=on,type,freq,q,gain" ... "b5=..." (login)
//                            type: 0 peaking, 1 low shelf, 2 high shelf
//   POST /eq/reset           back to the starter preset (login)
#include "sdkconfig.h"
#if CONFIG_AB_FEATURE_EQ
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eq.h"
#include "esp_http_server.h"
#include "ota_http.h"

static esp_err_t eq_get_handler(httpd_req_t *req)
{
    eq_config_t c = eq_get();
    static char json[1400];
    int n = snprintf(json, sizeof json, "{\"enabled\":%s,\"preamp_db\":%.2f,\"bands\":[", c.enabled ? "true" : "false",
                     eq_preamp_db());
    for (int i = 0; i < EQ_MAX_BANDS && n < (int)sizeof json - 120; i++) {
        const eq_band_t *b = &c.band[i];
        n += snprintf(json + n, sizeof json - n, "%s{\"on\":%d,\"type\":%d,\"f\":%.1f,\"q\":%.2f,\"g\":%.1f}", i ? "," : "",
                      b->on ? 1 : 0, (int)b->type, b->f0, b->q, b->gain_db);
    }
    snprintf(json + n, sizeof json - n, "]}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t eq_post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    static char body[640];
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

    eq_config_t c = eq_get();
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        int on, type, idx;
        float f, q, g;
        if (sscanf(line, "enabled=%d", &on) == 1) {
            c.enabled = on != 0;
        } else if (sscanf(line, "b%d=%d,%d,%f,%f,%f", &idx, &on, &type, &f, &q, &g) == 6 && idx >= 0 && idx < EQ_MAX_BANDS) {
            c.band[idx] = (eq_band_t){ on != 0, (eq_type_t)type, f, q, g };
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "cannot parse a line\n");
        }
    }
    if (!eq_set(&c)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "rejected: value out of range (f 20-20000, Q 0.2-10, gain -15..+6, type 0-2)\n");
    }
    return eq_get_handler(req);
}

static esp_err_t eq_reset_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    eq_reset_default();
    return eq_get_handler(req);
}

void eq_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/eq",       .method = HTTP_GET,  .handler = eq_get_handler },
        { .uri = "/eq",       .method = HTTP_POST, .handler = eq_post_handler },
        { .uri = "/eq/reset", .method = HTTP_POST, .handler = eq_reset_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#endif  // CONFIG_AB_FEATURE_EQ
