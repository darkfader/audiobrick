// Wi-Fi client with remembered networks. See wifi.h for the rules.
#include "wifi.h"
#include "sdkconfig.h"

#if CONFIG_AB_FEATURE_WIFI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "net.h"
#include "ota_http.h"

static const char *TAG = "wifi";

#define MAX_KNOWN      8
#define MAX_SCAN       20
#define RETRY_MS       8000     // wait between a lost link (or a failed attempt) and the next scan
#define SLOW_RETRY_MS  30000    // after several failures in a row (wrong password, network gone)

typedef struct {
    char ssid[33];
    char pass[65];
} known_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} seen_t;

static known_t s_known[MAX_KNOWN];
static int s_known_n;
static bool s_enabled;           // setting (flash)
static bool s_running;           // the driver is up
static bool s_connected;
static bool s_connecting;
static bool s_scanning;          // a scan requested from the page is running
static char s_ssid[33];          // network we are on
static int s_fail;
static char s_force[33];         // network chosen by hand with /wifi/connect ("" = automatic)
static seen_t s_seen[MAX_SCAN];
static int s_seen_n;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;

// ---- settings -------------------------------------------------------------------------------------------------------

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0;
    if (nvs_get_u8(h, "wifi_on", &on) == ESP_OK) s_enabled = on != 0;
    size_t len = sizeof s_known;
    if (nvs_get_blob(h, "wifi_nets", s_known, &len) != ESP_OK || len != sizeof s_known) {
        memset(s_known, 0, sizeof s_known);
    }
    nvs_close(h);
    s_known_n = 0;
    while (s_known_n < MAX_KNOWN && s_known[s_known_n].ssid[0]) s_known_n++;
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "wifi_on", s_enabled ? 1 : 0);
    nvs_set_blob(h, "wifi_nets", s_known, sizeof s_known);
    nvs_commit(h);
    nvs_close(h);
}

static int find_known(const char *ssid)
{
    for (int i = 0; i < s_known_n; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

// ---- connecting -----------------------------------------------------------------------------------------------------

static void start_scan(void)
{
    if (!s_running) return;
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(e));
        if (!s_connected) esp_timer_start_once(s_retry_timer, (int64_t)RETRY_MS * 1000);
    }
}

static void retry_cb(void *arg) { if (s_running && !s_connected && !s_connecting) start_scan(); }

static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (int64_t)(s_fail >= 3 ? SLOW_RETRY_MS : RETRY_MS) * 1000);
}

static void connect_to(const known_t *k)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, k->ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, k->pass, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = k->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_connecting = true;
    ESP_LOGI(TAG, "joining \"%s\"", k->ssid);
    esp_wifi_connect();
}

// After a scan: join the forced network if it is in range, otherwise the strongest remembered one.
static void pick_and_connect(void)
{
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++) {   // pass 0: the network chosen by hand, if in range; pass 1: strongest remembered
        int best_rssi = -128;
        for (int i = 0; i < s_seen_n; i++) {
            int k = find_known(s_seen[i].ssid);
            if (k < 0) continue;
            if (pass == 0) {
                if (s_force[0] && strcmp(s_seen[i].ssid, s_force) == 0) { best = k; break; }
            } else if (s_seen[i].rssi > best_rssi) {
                best_rssi = s_seen[i].rssi;
                best = k;
            }
        }
    }
    if (best >= 0) {
        connect_to(&s_known[best]);
    } else {
        s_fail++;
        schedule_retry();
    }
}

static void on_scan_done(void)
{
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *rec = malloc(sizeof *rec * MAX_SCAN);
    if (!rec) return;
    if (esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        s_seen_n = 0;
        for (int i = 0; i < n; i++) {   // records come strongest first; one entry per name
            if (!rec[i].ssid[0]) continue;
            bool dup = false;
            for (int j = 0; j < s_seen_n; j++) if (strcmp(s_seen[j].ssid, (char *)rec[i].ssid) == 0) dup = true;
            if (dup) continue;
            strlcpy(s_seen[s_seen_n].ssid, (char *)rec[i].ssid, sizeof s_seen[0].ssid);
            s_seen[s_seen_n].rssi = rec[i].rssi;
            s_seen[s_seen_n].secure = rec[i].authmode != WIFI_AUTH_OPEN;
            s_seen_n++;
        }
    }
    free(rec);
    s_scanning = false;
    if (!s_connected && !s_connecting) pick_and_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        start_scan();
        break;
    case WIFI_EVENT_SCAN_DONE:
        on_scan_done();
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *ev = data;
        s_connected = true;
        s_connecting = false;
        s_fail = 0;
        memcpy(s_ssid, ev->ssid, ev->ssid_len);
        s_ssid[ev->ssid_len] = '\0';
        ESP_LOGI(TAG, "connected to \"%s\"", s_ssid);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected (reason %d)", ev->reason);
        if (!s_connected) s_fail++;
        s_connected = false;
        s_connecting = false;
        s_ssid[0] = '\0';
        if (s_running) schedule_retry();
        break;
    }
    default:
        break;
    }
}

// ---- driver on/off --------------------------------------------------------------------------------------------------

static bool driver_start(void)
{
    if (s_running) return true;
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) return false;
        esp_netif_set_hostname(s_netif, "audiobrick");
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
        const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
        esp_timer_create(&ta, &s_retry_timer);
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;   // our own settings live in NVS; no need for the driver to save its copy
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_fail = 0;
    s_running = true;
    if (esp_wifi_start() != ESP_OK) {
        s_running = false;
        esp_wifi_deinit();
        return false;
    }
    ESP_LOGI(TAG, "started");
    return true;
}

static void driver_stop(void)
{
    if (!s_running) return;
    s_running = false;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();   // gives the memory back
    s_connected = s_connecting = s_scanning = false;
    s_ssid[0] = '\0';
    ESP_LOGI(TAG, "stopped");
}

void wifi_client_init(void)
{
    load_settings();
    if (s_enabled && s_known_n > 0 && !driver_start()) ESP_LOGW(TAG, "could not start");
}

// ---- web ------------------------------------------------------------------------------------------------------------

// Copies a string into JSON, escaping quotes, backslashes and control characters.
static size_t json_str(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        else dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return o;
}


static int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

static esp_err_t send_state(httpd_req_t *req)
{
    char *json = malloc(1600);
    if (!json) return httpd_resp_send_500(req);
    char esc[140];
    json_str(esc, sizeof esc, s_ssid);
    int o = snprintf(json, 1600, "{\"enabled\":%s,\"running\":%s,\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"known\":[",
                     s_enabled ? "true" : "false", s_running ? "true" : "false", s_connected ? "true" : "false", esc, net_wifi_ip_str(), s_connected ? wifi_rssi() : 0);
    for (int i = 0; i < s_known_n && o < 1400; i++) {
        json_str(esc, sizeof esc, s_known[i].ssid);
        o += snprintf(json + o, 1600 - o, "%s{\"ssid\":\"%s\",\"connected\":%s}", i ? "," : "", esc,
                      (s_connected && strcmp(s_known[i].ssid, s_ssid) == 0) ? "true" : "false");
    }
    snprintf(json + o, 1600 - o, "]}\n");
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, json);
    free(json);
    return e;
}

static esp_err_t get_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    return send_state(req);
}

// Reads a small text body; returns its length (0 on error or empty).
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) return 0;
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return 0;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

static esp_err_t bad(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, msg);
}

// POST /wifi?on=0|1
static esp_err_t post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char q[16], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "on", v, sizeof v) == ESP_OK) {
        bool on = atoi(v) != 0;
        if (on && s_known_n == 0) return bad(req, "409 Conflict", "add a network first\n");
        s_enabled = on;
        save_settings();
        if (on) { if (!driver_start()) return bad(req, "500 Internal Server Error", "Wi-Fi could not start (not enough memory?)\n"); }
        else driver_stop();
    }
    return send_state(req);
}

// POST /wifi/scan, then GET /wifi/scan
static esp_err_t scan_post(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    if (!s_running) return bad(req, "409 Conflict", "Wi-Fi is off\n");
    s_scanning = true;
    if (esp_wifi_scan_start(&(wifi_scan_config_t){ .show_hidden = false }, false) != ESP_OK) {
        s_scanning = false;
        return bad(req, "409 Conflict", "a scan or a connection attempt is already running, try again in a moment\n");
    }
    return httpd_resp_sendstr(req, "scanning, then GET /wifi/scan\n");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char *json = malloc(2600);
    if (!json) return httpd_resp_send_500(req);
    int o = snprintf(json, 2600, "{\"scanning\":%s,\"networks\":[", s_scanning ? "true" : "false");
    char esc[140];
    for (int i = 0; i < s_seen_n && o < 2350; i++) {
        json_str(esc, sizeof esc, s_seen[i].ssid);
        o += snprintf(json + o, 2600 - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s,\"known\":%s}", i ? "," : "", esc, s_seen[i].rssi,
                      s_seen[i].secure ? "true" : "false", find_known(s_seen[i].ssid) >= 0 ? "true" : "false");
    }
    snprintf(json + o, 2600 - o, "]}\n");
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, json);
    free(json);
    return e;
}

// POST /wifi/add  body "ssid\npassword"
static esp_err_t add_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[120];
    if (!read_body(req, body, sizeof body)) return bad(req, "400 Bad Request", "body: ssid, newline, password\n");
    char *pass = strchr(body, '\n');
    if (pass) *pass++ = '\0'; else pass = body + strlen(body);
    char *cr = strchr(pass, '\r'); if (cr) *cr = '\0';
    cr = strchr(body, '\r'); if (cr) *cr = '\0';
    size_t sl = strlen(body), pl = strlen(pass);
    if (sl == 0 || sl > 32) return bad(req, "400 Bad Request", "network name must be 1 to 32 characters\n");
    if (pl != 0 && (pl < 8 || pl > 64)) return bad(req, "400 Bad Request", "password must be 8 to 63 characters (or empty for an open network)\n");
    int k = find_known(body);
    if (k < 0) {
        if (s_known_n >= MAX_KNOWN) return bad(req, "409 Conflict", "8 networks are remembered already; forget one first\n");
        k = s_known_n++;
    }
    strlcpy(s_known[k].ssid, body, sizeof s_known[k].ssid);
    strlcpy(s_known[k].pass, pass, sizeof s_known[k].pass);
    s_enabled = true;
    save_settings();
    if (!driver_start()) return bad(req, "500 Internal Server Error", "Wi-Fi could not start (not enough memory?)\n");
    // try the new network right away, also when we are already on another one
    strlcpy(s_force, s_known[k].ssid, sizeof s_force);
    s_fail = 0;
    if (s_connected) { esp_wifi_disconnect(); } else if (!s_connecting) start_scan();
    return send_state(req);
}

// POST /wifi/forget  body = ssid
static esp_err_t forget_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[40];
    if (!read_body(req, body, sizeof body)) return bad(req, "400 Bad Request", "body = network name\n");
    int k = find_known(body);
    if (k < 0) return bad(req, "404 Not Found", "not remembered\n");
    bool was_current = s_connected && strcmp(s_ssid, body) == 0;
    for (int i = k; i < s_known_n - 1; i++) s_known[i] = s_known[i + 1];
    memset(&s_known[--s_known_n], 0, sizeof s_known[0]);
    if (strcmp(s_force, body) == 0) s_force[0] = '\0';
    if (s_known_n == 0) s_enabled = false;
    save_settings();
    if (s_known_n == 0) driver_stop();
    else if (was_current) esp_wifi_disconnect();   // the retry logic picks another remembered network
    return send_state(req);
}

// POST /wifi/connect  body = ssid
static esp_err_t connect_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[40];
    if (!read_body(req, body, sizeof body)) return bad(req, "400 Bad Request", "body = network name\n");
    if (find_known(body) < 0) return bad(req, "404 Not Found", "not remembered; add it first\n");
    if (!s_running) return bad(req, "409 Conflict", "Wi-Fi is off\n");
    strlcpy(s_force, body, sizeof s_force);
    s_fail = 0;
    if (s_connected) esp_wifi_disconnect(); else if (!s_connecting) start_scan();
    return send_state(req);
}

void wifi_client_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/wifi",         .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/wifi",         .method = HTTP_POST, .handler = post_handler },
        { .uri = "/wifi/scan",    .method = HTTP_POST, .handler = scan_post },
        { .uri = "/wifi/scan",    .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/wifi/add",     .method = HTTP_POST, .handler = add_handler },
        { .uri = "/wifi/forget",  .method = HTTP_POST, .handler = forget_handler },
        { .uri = "/wifi/connect", .method = HTTP_POST, .handler = connect_handler },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#else  // feature switched off at build time

void wifi_client_init(void) {}
void wifi_client_http_register(httpd_handle_t server) { (void)server; }

#endif
