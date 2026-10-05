// Bluetooth A2DP sink for the Audio Brick (ESP32 classic Bluetooth, Bluedroid host stack).
// The stack decodes SBC and hands us 16-bit stereo PCM at 44.1 or 48 kHz; it is mixed into the main player slot like the
// network inputs. See bluetooth.h for the pairing rules.
#include "sdkconfig.h"
#include "bluetooth.h"

#if CONFIG_AB_FEATURE_BLUETOOTH

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "media.h"
#include "nvs.h"
#include "ota_http.h"

static const char *TAG = "bt";

#define PAIRING_WINDOW_S 120
#define BOOT_PAIRING_WINDOW_S 180
#define BT_MIN_PREBUFFER_MS 120   // A2DP delivers in bursts; a 50 ms buffer (the global low-latency default) would run dry between them
#define LATENCY_HEADROOM_MS 180   // drop incoming audio when the buffer is this far above the pre-buffer (source clock runs fast)

typedef struct {
    bool enabled;        // setting (flash): visible / connectable
    bool pair_boot;      // setting (flash): open the pairing window for a few minutes after every start
    bool ssp;            // setting (flash): pair with Secure Simple Pairing, or (default) with the PIN 0000; changing it restarts the board
    bool stack_up;
    bool pairing;
    bool connected;
    bool ours;           // we hold the main channel
    bool blocked;        // the user pressed Stop: ignore the source until it pauses or starts again
    bool audio_started;
    uint32_t rate;
    uint32_t packets, dropped, bytes;
    uint32_t underruns;  // dropouts at the speaker, summed over all sessions since the Brick started
    uint32_t ur_session; // the part of media_underruns() already added above
    char name[32];       // our name, as seen by phones
    char peer[18];       // address of the connected device
    char peer_name[40];
    int64_t last_data_us;
    int rssi_delta;      // last link signal reading (dB relative to the golden receive range)
} bt_state_t;

#define SCAN_MAX 16
typedef struct { char addr[18]; char name[32]; int rssi; } scan_hit_t;
static scan_hit_t s_scan[SCAN_MAX];
static int s_scan_n;
static bool s_scanning;

static bt_state_t s = { .name = "Audio Brick" };
static esp_timer_handle_t s_pair_timer, s_conn_timer;
static uint8_t s_last_peer[6];       // device that was connected last (saved in flash): the Brick connects back to it
static bool s_have_last;
static int s_conn_tries;             // reconnect attempts left (after a start or after pairing)
static resampler_t s_rs;
static uint32_t s_rs_rate;
static EXT_RAM_BSS_ATTR int16_t s_out[2 * 2048];   // resampled frames (PSRAM: internal RAM is tight with the Bluetooth controller)

// ---- settings in flash ----------------------------------------------------------------------------------------------

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bt_on", s.enabled);
    nvs_set_u8(h, "bt_pairboot", s.pair_boot);
    nvs_set_u8(h, "bt_ssp", s.ssp);
    nvs_set_str(h, "bt_name", s.name);
    nvs_commit(h);
    nvs_close(h);
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0, pb = 1, ssp = 1;
    if (nvs_get_u8(h, "bt_on", &on) == ESP_OK) s.enabled = on != 0;
    if (nvs_get_u8(h, "bt_pairboot", &pb) == ESP_OK) s.pair_boot = pb != 0; else s.pair_boot = true;   // default: on
    if (nvs_get_u8(h, "bt_ssp", &ssp) == ESP_OK) s.ssp = ssp != 0; else s.ssp = false;   // default: PIN 0000 (works with Windows and phones)
    size_t blen = sizeof s_last_peer;
    s_have_last = nvs_get_blob(h, "bt_last", s_last_peer, &blen) == ESP_OK && blen == sizeof s_last_peer;
    size_t len = sizeof s.name;
    char name[sizeof s.name];
    if (nvs_get_str(h, "bt_name", name, &len) == ESP_OK && name[0]) strlcpy(s.name, name, sizeof s.name);
    nvs_close(h);
}

// ---- audio path -----------------------------------------------------------------------------------------------------

static void release_channel(void)
{
    if (s.ours) {
        media_finish();  // let the buffer drain
        s.ours = false;
    }
}

// Called by the Bluetooth stack with decoded PCM (16-bit interleaved stereo at s.rate).
static void data_cb(const uint8_t *data, uint32_t len)
{
    size_t n = len / 4;
    if (n == 0 || s.rate == 0) return;
    s.packets++;
    s.bytes += len;
    s.last_data_us = esp_timer_get_time();
    if (s.blocked) return;
    if (s.ours && media_aborted()) {  // Stop was pressed on the page
        s.ours = false;
        s.blocked = true;
        return;
    }
    if (!s.ours) {
        char label[64];
        snprintf(label, sizeof label, "bluetooth: %s", s.peer_name[0] ? s.peer_name : s.peer);
        if (media_active() || !media_begin(MEDIA_STREAM, label)) {  // something else is playing: first come, first served
            s.dropped++;
            return;
        }
        s.ours = true;
        s.ur_session = 0;
        media_set_slot_min_prebuffer_ms(SLOT_MAIN, BT_MIN_PREBUFFER_MS);
        s_rs_rate = 0;
    }
    uint32_t ur = media_underruns();   // count dropouts across sessions (media resets its own counter at every start)
    if (ur > s.ur_session) {
        s.underruns += ur - s.ur_session;
        s.ur_session = ur;
    }
    if (media_buffer_ms() > (media_prebuffer_ms() > BT_MIN_PREBUFFER_MS ? media_prebuffer_ms() : BT_MIN_PREBUFFER_MS) + LATENCY_HEADROOM_MS) {
        s.dropped++;
        return;
    }
    const int16_t *frames = (const int16_t *)data;  // the stack hands out aligned buffers
    if (s.rate == 48000) {
        media_write_nb_slot(SLOT_MAIN, frames, n);
        return;
    }
    if (s.rate != s_rs_rate) {
        resampler_init(&s_rs, s.rate);
        s_rs_rate = s.rate;
    }
    for (size_t done = 0; done < n;) {
        size_t slice = n - done > 256 ? 256 : n - done;
        size_t produced = resampler_process(&s_rs, frames + 2 * done, slice, s_out, 2048);
        if (produced) media_write_nb_slot(SLOT_MAIN, s_out, produced);
        done += slice;
    }
}

// ---- pairing window -------------------------------------------------------------------------------------------------

static void apply_scan_mode(void)
{
    if (!s.stack_up) return;
    if (!s.enabled) {
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    } else {
        esp_err_t e = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, s.pairing ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
        if (e != ESP_OK) ESP_LOGW(TAG, "set_scan_mode failed: %s", esp_err_to_name(e));
    }
}

static void pair_timeout(void *arg)
{
    s.pairing = false;
    apply_scan_mode();
    ESP_LOGI(TAG, "pairing window closed");
}

// Drops the connected device (if any) and stops connect-back, so it does not grab the link again at once.
static void disconnect_peer(void)
{
    s_conn_tries = 0;
    if (!s.connected || !s.stack_up) return;
    esp_bd_addr_t a;
    unsigned b[6];
    if (sscanf(s.peer, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return;
    for (int i = 0; i < 6; i++) a[i] = (uint8_t)b[i];
    esp_a2d_sink_disconnect(a);
}

static void set_pairing(bool on)
{
    s.pairing = on && s.enabled;
    if (s.pairing) disconnect_peer();   // a new pairing window means a new device: free the single link
    esp_timer_stop(s_pair_timer);
    if (s.pairing) esp_timer_start_once(s_pair_timer, (int64_t)PAIRING_WINDOW_S * 1000000);
    apply_scan_mode();
    ESP_LOGI(TAG, "pairing window %s", s.pairing ? "open" : "closed");
}

// ---- stack callbacks ------------------------------------------------------------------------------------------------

static void addr_str(const uint8_t *a, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void remember_peer(const uint8_t *bda)
{
    memcpy(s_last_peer, bda, 6);
    s_have_last = true;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bt_last", s_last_peer, sizeof s_last_peer);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Connect back to the device we were last connected to, like a pair of headphones does. Tries a few times (the other side may still be starting).
static void conn_tick(void *arg)
{
    if (!s.stack_up || !s.enabled || s.connected || !s_have_last || s_conn_tries <= 0) {
        esp_timer_stop(s_conn_timer);
        return;
    }
    s_conn_tries--;
    esp_bd_addr_t a;
    memcpy(a, s_last_peer, 6);
    ESP_LOGI(TAG, "connecting back to the last device (%d tries left)", s_conn_tries);
    esp_a2d_sink_connect(a);
}

static void start_connect_back(int tries, int64_t first_delay_us)
{
    s_conn_tries = tries;
    esp_timer_stop(s_conn_timer);
    esp_timer_start_periodic(s_conn_timer, 15 * 1000000LL);
    if (first_delay_us > 0 && s_have_last) {   // do not wait a full period for the first attempt
        esp_timer_stop(s_conn_timer);
        esp_timer_start_once(s_conn_timer, first_delay_us);
    }
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:   // "just works" pairing: only accepted while the pairing window is open
        ESP_LOGI(TAG, "pairing request, window %s", s.pairing ? "open: accepted" : "closed: refused");
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, s.pairing);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", p->auth_cmpl.device_name);
            strlcpy(s.peer_name, (const char *)p->auth_cmpl.device_name, sizeof s.peer_name);
            if (s.pairing) set_pairing(false);  // one device per window
            remember_peer(p->auth_cmpl.bda);
            start_connect_back(3, 2000000);       // the paired device usually waits for the sink to connect
        } else {
            ESP_LOGW(TAG, "authentication failed (%d)", p->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            strlcpy(s.peer_name, (const char *)p->read_rmt_name.rmt_name, sizeof s.peer_name);
        }
        break;
    case ESP_BT_GAP_DISC_RES_EVT: {   // result of POST /bluetooth/scan: who can the Brick hear, and how strongly?
        scan_hit_t h = { .rssi = -127 };
        addr_str(p->disc_res.bda, h.addr);
        for (int i = 0; i < p->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *d = &p->disc_res.prop[i];
            if (d->type == ESP_BT_GAP_DEV_PROP_RSSI) h.rssi = *(int8_t *)d->val;
            else if (d->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                size_t l = d->len < sizeof h.name - 1 ? d->len : sizeof h.name - 1;
                memcpy(h.name, d->val, l);
                h.name[l] = '\0';
            }
        }
        for (int i = 0; i < s_scan_n; i++) {
            if (!strcmp(s_scan[i].addr, h.addr)) {   // update an already listed device
                if (h.name[0]) strlcpy(s_scan[i].name, h.name, sizeof h.name);
                if (h.rssi != -127) s_scan[i].rssi = h.rssi;
                return;
            }
        }
        if (s_scan_n < SCAN_MAX) s_scan[s_scan_n++] = h;
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_scanning = p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
        ESP_LOGI(TAG, "scan %s, %d devices", s_scanning ? "started" : "finished", s_scan_n);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:   // a device connected on the radio level: how strong is its signal?
        if (p->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) esp_bt_gap_read_rssi_delta(p->acl_conn_cmpl_stat.bda);
        break;
    case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
        s.rssi_delta = p->read_rssi_delta.rssi_delta;
        ESP_LOGI(TAG, "link signal: %d dB relative to the ideal receive range (0 is ideal, below -20 is weak)", s.rssi_delta);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {  // legacy PIN pairing (only when SSP is switched off): the PIN is 0000, accepted only in the pairing window
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        ESP_LOGI(TAG, "PIN request, window %s", s.pairing ? "open: accepted" : "closed: refused");
        esp_bt_gap_pin_reply(p->pin_req.bda, s.pairing, 4, pin);
        break;
    }
    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            addr_str(p->conn_stat.remote_bda, s.peer);
            s.connected = true;
            // Tell the source how long the Brick takes from receiving audio to the speaker (units of 0.1 ms): the pre-buffer plus the
            // output stage (about 37 ms). Phones that support AVDTP delay reporting use it to keep video in sync with the sound.
            esp_a2d_sink_set_delay_value((uint16_t)(((media_prebuffer_ms() > BT_MIN_PREBUFFER_MS ? media_prebuffer_ms() : BT_MIN_PREBUFFER_MS) + 40) * 10));
            remember_peer(p->conn_stat.remote_bda);
            s.peer_name[0] = '\0';
            esp_bt_gap_read_remote_name(p->conn_stat.remote_bda);
            ESP_LOGI(TAG, "connected: %s", s.peer);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "disconnected");
            s.connected = false;
            s.audio_started = false;
            s.blocked = false;
            release_channel();
            s.peer[0] = '\0';
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        s.audio_started = p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        s.blocked = false;   // a new play/pause from the phone clears a Stop from the page
        if (!s.audio_started) release_channel();
        ESP_LOGI(TAG, "audio %s", s.audio_started ? "started" : "suspended");
        break;
    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_mcc_t *m = &p->audio_cfg.mcc;
        if (m->type == ESP_A2D_MCT_SBC) {
            uint8_t f = m->cie.sbc_info.samp_freq;
            s.rate = (f & ESP_A2D_SBC_CIE_SF_48K) ? 48000 : (f & ESP_A2D_SBC_CIE_SF_44K) ? 44100 :
                     (f & ESP_A2D_SBC_CIE_SF_32K) ? 32000 : 16000;
            ESP_LOGI(TAG, "SBC %u Hz, bitpool %d-%d", (unsigned)s.rate, m->cie.sbc_info.min_bitpool, m->cie.sbc_info.max_bitpool);
        }
        break;
    }
    default:
        break;
    }
}

// ---- AVRCP (remote control) ------------------------------------------------------------------------------------------
// Windows and most phones expect a speaker to offer both A2DP and AVRCP; without the AVRCP service record Windows pairs but never sets up
// the audio link. We do not use the controls yet, so the callbacks only exist because the stack wants them.

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p)
{
    (void)event;
    (void)p;
}

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *p)
{
    (void)event;
    (void)p;
}

// ---- stack start ----------------------------------------------------------------------------------------------------

static bool stack_start(void)
{
    if (s.stack_up) return true;
    // Classic Bluetooth only: give the BLE half of the controller memory back to the heap.
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(TAG, "controller start failed");
        return false;
    }
    esp_bluedroid_config_t bd = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bd.ssp_en = s.ssp;   // off = legacy PIN pairing: no "man-in-the-middle protection" requirement on the audio service, see docs/bluetooth.md
    if (esp_bluedroid_init_with_cfg(&bd) != ESP_OK || esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid start failed");
        return false;
    }
    if (s.ssp) {
        esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;   // no screen or keys: "just works", guarded by the pairing window
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof iocap);
    } else {
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);   // answered in the PIN_REQ event, so it can be refused outside the pairing window
    }
    esp_bt_gap_set_device_name(s.name);
    esp_bt_gap_register_callback(gap_cb);
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(data_cb);
    if (esp_a2d_sink_init() != ESP_OK) {
        ESP_LOGE(TAG, "A2DP init failed");
        return false;
    }
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(avrc_ct_cb);
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(avrc_tg_cb);
    // Class of device: audio / loudspeaker, so phones list it with a speaker icon.
    esp_bt_cod_t cod = { .major = ESP_BT_COD_MAJOR_DEV_AV, .minor = 0x05 };
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    s.stack_up = true;
    apply_scan_mode();
    ESP_LOGI(TAG, "Bluetooth audio ready as \"%s\" (free internal heap %u)", s.name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void bluetooth_init(void)
{
    const esp_timer_create_args_t t = { .callback = pair_timeout, .name = "bt_pair" };
    esp_timer_create(&t, &s_pair_timer);
    const esp_timer_create_args_t c = { .callback = conn_tick, .name = "bt_conn" };
    esp_timer_create(&c, &s_conn_timer);
    load_settings();
    if (s.enabled && stack_start()) {
        start_connect_back(8, 4000000);          // about two minutes of tries after a start
        if (s.pair_boot) {                       // a new device can be paired for a few minutes after every start
            s.pairing = true;
            esp_timer_start_once(s_pair_timer, (int64_t)BOOT_PAIRING_WINDOW_S * 1000000);
            apply_scan_mode();
            ESP_LOGI(TAG, "pairing window open for %d s after start", BOOT_PAIRING_WINDOW_S);
        }
    }
}

static void restart_cb(void *arg)
{
    esp_restart();
}

// ---- HTTP -----------------------------------------------------------------------------------------------------------

static esp_err_t get_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);   // shows paired device names: login only
    char bonded[200] = "[";
    if (s.stack_up) {
        int n = esp_bt_gap_get_bond_device_num();
        if (n > 8) n = 8;
        esp_bd_addr_t list[8];
        if (n > 0 && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                char a[18];
                addr_str(list[i], a);
                size_t l = strlen(bonded);
                snprintf(bonded + l, sizeof bonded - l, "%s\"%s\"", i ? "," : "", a);
            }
        }
    }
    strlcat(bonded, "]", sizeof bonded);
    char json[640];
    snprintf(json, sizeof json,
             "{\"enabled\":%s,\"ssp\":%s,\"pair_boot\":%s,\"stack_up\":%s,\"name\":\"%s\",\"pairing\":%s,\"connected\":%s,\"peer\":\"%s\",\"peer_name\":\"%s\","
             "\"playing\":%s,\"rate\":%u,\"packets\":%u,\"dropped\":%u,\"underruns\":%u,\"rssi_delta\":%d,\"bonded\":%s,\"free_heap\":%u}\n",
             s.enabled ? "true" : "false", s.ssp ? "true" : "false", s.pair_boot ? "true" : "false", s.stack_up ? "true" : "false", s.name, s.pairing ? "true" : "false",
             s.connected ? "true" : "false", s.peer, s.peer_name, s.ours ? "true" : "false", (unsigned)s.rate,
             (unsigned)s.packets, (unsigned)s.dropped, (unsigned)s.underruns, s.rssi_delta, bonded, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /bluetooth?on=1|0&name=Audio%20Brick
static esp_err_t post_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char q[96], v[48];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "name", v, sizeof v) == ESP_OK) {
            size_t j = 0;
            for (size_t i = 0; v[i] && j < sizeof s.name - 1; i++) {   // letters, digits, space, dash, underscore only
                char c = v[i];
                if (c == '+') c = ' ';
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_') s.name[j++] = c;
            }
            s.name[j] = '\0';
            if (j == 0) strlcpy(s.name, "Audio Brick", sizeof s.name);
            if (s.stack_up) esp_bt_gap_set_device_name(s.name);
        }
        if (httpd_query_key_value(q, "pairboot", v, sizeof v) == ESP_OK) s.pair_boot = atoi(v) != 0;
        if (httpd_query_key_value(q, "ssp", v, sizeof v) == ESP_OK && (atoi(v) != 0) != s.ssp) {
            s.ssp = atoi(v) != 0;   // the pairing method is chosen when the stack starts: save it and restart
            save_settings();
            esp_timer_handle_t t;
            const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "bt_restart" };
            if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 1500000);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"restarting\":true}\n");
        }
        if (httpd_query_key_value(q, "on", v, sizeof v) == ESP_OK) {
            s.enabled = atoi(v) != 0;
            if (s.enabled && !s.stack_up && !stack_start()) {
                s.enabled = false;
                save_settings();
                httpd_resp_set_status(req, "500 Internal Server Error");
                return httpd_resp_sendstr(req, "Bluetooth could not start (not enough memory?)\n");
            }
            if (!s.enabled) {
                s.pairing = false;
                esp_timer_stop(s_pair_timer);
                disconnect_peer();
            }
            apply_scan_mode();
        }
        save_settings();
    }
    return get_handler(req);
}

// POST /bluetooth/pair?on=1  opens the pairing window (2 minutes); on=0 closes it
static esp_err_t pair_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char q[16], v[8];
    bool on = true;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "on", v, sizeof v) == ESP_OK) on = atoi(v) != 0;
    if (!s.stack_up) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "switch Bluetooth on first\n");
    }
    set_pairing(on);
    return get_handler(req);
}

// POST /bluetooth/disconnect  drops the connected device (it stays paired; use connect to bring it back)
static esp_err_t disconnect_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    disconnect_peer();
    return get_handler(req);
}

// POST /bluetooth/forget  removes every paired device
static esp_err_t forget_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    if (s.stack_up) {
        int n = esp_bt_gap_get_bond_device_num();
        if (n > 8) n = 8;
        esp_bd_addr_t list[8];
        if (n > 0 && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
            for (int i = 0; i < n; i++) esp_bt_gap_remove_bond_device(list[i]);
        }
    }
    return get_handler(req);
}

// POST /bluetooth/scan: look around for 10 s (diagnostics: shows whether the radio and antenna hear anything). GET lists what was heard.
static esp_err_t scan_post(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    if (!s.stack_up) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "switch Bluetooth on first\n");
    }
    s_scan_n = 0;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);   // 8 x 1.28 s
    return httpd_resp_sendstr(req, "scanning for 10 s, then GET /bluetooth/scan\n");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char json[900] = "{\"scanning\":";
    strlcat(json, s_scanning ? "true" : "false", sizeof json);
    strlcat(json, ",\"devices\":[", sizeof json);
    for (int i = 0; i < s_scan_n; i++) {
        char one[100];
        snprintf(one, sizeof one, "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d}", i ? "," : "", s_scan[i].addr, s_scan[i].name, s_scan[i].rssi);
        strlcat(json, one, sizeof json);
    }
    strlcat(json, "]}\n", sizeof json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// POST /bluetooth/connect: connect to the device that was connected last (it has to be paired and in range).
static esp_err_t connect_handler(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    if (!s.stack_up || !s.enabled || !s_have_last) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Bluetooth is off, or no device has been paired yet\n");
    }
    start_connect_back(3, 500000);
    return get_handler(req);
}

void bluetooth_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/bluetooth",        .method = HTTP_GET,  .handler = get_handler },
        { .uri = "/bluetooth",        .method = HTTP_POST, .handler = post_handler },
        { .uri = "/bluetooth/pair",   .method = HTTP_POST, .handler = pair_handler },
        { .uri = "/bluetooth/forget", .method = HTTP_POST, .handler = forget_handler },
        { .uri = "/bluetooth/connect", .method = HTTP_POST, .handler = connect_handler },
        { .uri = "/bluetooth/disconnect", .method = HTTP_POST, .handler = disconnect_handler },
        { .uri = "/bluetooth/scan",   .method = HTTP_POST, .handler = scan_post },
        { .uri = "/bluetooth/scan",   .method = HTTP_GET,  .handler = scan_get },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}

#else  // feature switched off at build time

void bluetooth_init(void) {}
void bluetooth_http_register(httpd_handle_t server) { (void)server; }

#endif
