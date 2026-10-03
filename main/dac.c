// TAS5825M control. The power-up register sequence follows the field-tested one from
// github.com/mrtoy-me/esphome-tas58xx (via rmalchow/ondaire tas58xx.c). Registers marked
// "vendor" have no datasheet meaning there and are copied as-is.
#include "dac.h"

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ota_http.h"
#include "speaker_limits.h"

static const char *TAG = "dac";

#define REG_PAGE      0x00
#define REG_RESET     0x01
#define REG_DEV_CTRL1 0x02  // BTL/PBTL, modulation
#define REG_DEV_CTRL2 0x03  // CTRL_STATE: 0 deep sleep, 2 Hi-Z, 3 play; bit 3 = mute
#define REG_DIG_VOL   0x4C  // 0x30 = 0 dB, -0.5 dB per LSB, 0xFF = mute
#define REG_AGAIN     0x54  // analog gain, 0x00 = 0 dB
#define REG_FAULT_CLR 0x78
#define REG_BOOK      0x7F

#define CTRL2_HIZ     0x02
#define CTRL2_PLAY    0x03
#define CTRL2_MUTE    0x08
#define DELAY_MARK    0xFE  // pseudo-register: value is a delay in ms

typedef struct { uint8_t reg, val; } regval_t;

static const regval_t INIT_SEQ[] = {
    { REG_PAGE,      0x00 },
    { REG_BOOK,      0x00 },
    { REG_DEV_CTRL2, 0x02 },  // Hi-Z
    { REG_RESET,     0x11 },  // reset core and registers
    { REG_DEV_CTRL2, 0x02 },
    { DELAY_MARK,    0x05 },
    { REG_DEV_CTRL2, 0x00 },  // deep sleep
    { 0x46,          0x11 },  // vendor: clock/sample-rate config (48 kHz)
    { REG_DEV_CTRL2, 0x02 },
    { 0x61,          0x0B },  // vendor
    { 0x60,          0x01 },  // vendor
    { 0x7D,          0x11 },  // vendor
    { 0x7E,          0xFF },  // vendor
    { REG_PAGE,      0x01 },
    { 0x51,          0x05 },  // vendor (page 1)
    { REG_PAGE,      0x00 },
    { REG_BOOK,      0x00 },
    { REG_DEV_CTRL1, 0x00 },  // BTL, normal modulation
    { 0x30,          0x00 },  // vendor
    { REG_DIG_VOL,   0x30 + (-DAC_DEFAULT_VOLUME_DB) * 2 },
    { 0x53,          0x00 },  // vendor
    { REG_AGAIN,     0x00 },
    { REG_DEV_CTRL2, CTRL2_PLAY | CTRL2_MUTE },  // play, but muted
    { REG_FAULT_CLR, 0x80 },
};

// Idle power saving: once the amp has been muted for hiz_s its outputs go Hi-Z (chip still on, wakes in a
// few ms); after off_s the chip is powered down through PWDN (wakes in about 30 ms). 0 = never.
typedef struct { uint16_t hiz_s, off_s; } power_cfg_t;
#define DEFAULT_HIZ_S 20
#define DEFAULT_OFF_S 600

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static int s_vol_db = DAC_DEFAULT_VOLUME_DB;
static amp_state_t s_state = AMP_ACTIVE;
static bool s_muted = true;
static int64_t s_muted_since_us;
static power_cfg_t s_cfg = { DEFAULT_HIZ_S, DEFAULT_OFF_S };
static SemaphoreHandle_t s_lock;
static uint32_t s_wakes_hiz, s_wakes_off;

static bool write_reg(uint8_t reg, uint8_t val)
{
    if (s_state == AMP_OFF) return false;  // chip unpowered; the volume is re-applied on power-up
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof buf, 50);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write 0x%02X=0x%02X failed: %s", reg, val, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

// Full chip power-up: PWDN cycle, register sequence, then the volume currently wanted. Leaves it playing, muted.
static bool chip_power_up(void)
{
    // TI power-up timing: low, >=1 ms, high, >=5 ms before I2C.
    gpio_set_level(PIN_DAC_PWDN, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_DAC_PWDN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_state = AMP_ACTIVE;
    for (size_t i = 0; i < sizeof INIT_SEQ / sizeof INIT_SEQ[0]; i++) {
        if (INIT_SEQ[i].reg == DELAY_MARK) {
            vTaskDelay(pdMS_TO_TICKS(INIT_SEQ[i].val));
        } else if (!write_reg(INIT_SEQ[i].reg, INIT_SEQ[i].val)) {
            return false;
        }
    }
    s_muted = true;
    s_muted_since_us = esp_timer_get_time();
    return dac_set_volume_db(s_vol_db);  // applies the profile's volume cap
}

static void power_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) != ESP_OK) return;
    power_cfg_t c;
    size_t len = sizeof c;
    if (nvs_get_blob(h, "power", &c, &len) == ESP_OK && len == sizeof c) s_cfg = c;
    nvs_close(h);
}

static void power_tick(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_muted && s_state != AMP_OFF) {
        int64_t idle_s = (esp_timer_get_time() - s_muted_since_us) / 1000000;
        if (s_cfg.off_s && idle_s >= s_cfg.off_s) {
            if (s_state == AMP_ACTIVE) write_reg(REG_DEV_CTRL2, CTRL2_HIZ);
            vTaskDelay(pdMS_TO_TICKS(5));
            gpio_set_level(PIN_DAC_PWDN, 0);
            s_state = AMP_OFF;
            ESP_LOGI(TAG, "amp powered down after %lld s idle", (long long)idle_s);
        } else if (s_cfg.hiz_s && idle_s >= s_cfg.hiz_s && s_state == AMP_ACTIVE) {
            write_reg(REG_DEV_CTRL2, CTRL2_HIZ);
            s_state = AMP_HIZ;
            ESP_LOGI(TAG, "amp Hi-Z after %lld s idle", (long long)idle_s);
        }
    }
    xSemaphoreGive(s_lock);
}

bool dac_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    power_load();
    gpio_config_t pwdn = {
        .pin_bit_mask = 1ULL << PIN_DAC_PWDN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwdn);

    // GPIO36/39 are input only with no internal pulls; the board pulls them up.
    gpio_config_t status = {
        .pin_bit_mask = (1ULL << PIN_DAC_FAULT) | (1ULL << PIN_DAC_WARN),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&status);

    gpio_set_level(PIN_DAC_PWDN, 1);  // the I2C probe below needs the chip powered
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }
    if (i2c_master_probe(bus, DAC_I2C_ADDR, 50) != ESP_OK) {
        ESP_LOGE(TAG, "no ACK from TAS5825M at 0x%02X", DAC_I2C_ADDR);
        return false;
    }
    s_bus = bus;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DAC_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed");
        return false;
    }

    s_vol_db = DAC_DEFAULT_VOLUME_DB;
    if (!chip_power_up()) return false;
    const esp_timer_create_args_t targs = { .callback = power_tick, .name = "amp_power" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&targs, &timer) == ESP_OK) esp_timer_start_periodic(timer, 500000);
    ESP_LOGI(TAG, "TAS5825M ready: BTL, %d dB (cap %d dB), muted; Hi-Z after %d s, off after %d s (0 = never)",
             s_vol_db, limits_max_volume_db(), s_cfg.hiz_s, s_cfg.off_s);
    return true;
}

bool dac_set_mute(bool mute)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = true;
    if (mute) {
        if (s_state != AMP_OFF) ok = write_reg(REG_DEV_CTRL2, CTRL2_PLAY | CTRL2_MUTE);
        if (ok && !s_muted) {
            s_muted = true;
            s_muted_since_us = esp_timer_get_time();
        }
        if (ok && s_state == AMP_HIZ) s_state = AMP_ACTIVE;  // the write above left Hi-Z; only the timer re-enters it
    } else {
        if (s_state == AMP_OFF) {
            s_wakes_off++;
            ok = chip_power_up();
        } else if (s_state == AMP_HIZ) {
            s_wakes_hiz++;
            // Hi-Z -> play passes through the muted play state so the output stage starts from silence.
            ok = write_reg(REG_DEV_CTRL2, CTRL2_PLAY | CTRL2_MUTE);
            if (ok) s_state = AMP_ACTIVE;
        }
        if (ok) ok = write_reg(REG_DEV_CTRL2, CTRL2_PLAY);
        if (ok) s_muted = false;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

amp_state_t dac_state(void) { return s_state; }

bool dac_power_set(int hiz_s, int off_s)
{
    if (hiz_s < 0 || hiz_s > 3600 || off_s < 0 || off_s > 86400) return false;
    if (hiz_s && off_s && off_s <= hiz_s) return false;
    power_cfg_t c = { (uint16_t)hiz_s, (uint16_t)off_s };
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "power", &c, sizeof c) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) s_cfg = c;
    return ok;
}

void dac_power_get(int *hiz_s, int *off_s)
{
    *hiz_s = s_cfg.hiz_s;
    *off_s = s_cfg.off_s;
}

bool dac_set_volume_db(int db)
{
    // The speaker profile caps the volume so even a 0 dBFS signal stays under the SPL limit.
    int cap = limits_max_volume_db();
    if (db > cap) db = cap;
    if (db < -90) db = -90;
    s_vol_db = db;
    return write_reg(REG_DIG_VOL, (uint8_t)(0x30 + (-db) * 2)) || s_state == AMP_OFF;
}

int dac_get_volume_db(void)
{
    return s_vol_db;
}

bool dac_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_state == AMP_OFF) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50) == ESP_OK;
}

float dac_pvdd_volts(void)
{
    uint8_t raw;
    if (!dac_read_reg(0x5E, &raw)) return -1.0f;
    return raw / 8.428f;  // datasheet: 223 = 26.45 V, 38 = 4.51 V
}

bool dac_clear_faults(void)
{
    return write_reg(REG_FAULT_CLR, 0x80);
}

bool dac_fault_active(void)   { return s_state != AMP_OFF && gpio_get_level(PIN_DAC_FAULT) == 0; }
bool dac_warning_active(void) { return s_state != AMP_OFF && gpio_get_level(PIN_DAC_WARN) == 0; }

// ---- HTTP ------------------------------------------------------------------------------------------
//   GET  /power    {"state","muted","hiz_after_s","off_after_s","wakes_from_hiz","wakes_from_off"} (no login)
//   POST /power    body "hiz_after_s=20" and "off_after_s=600" lines, seconds, 0 = never (login)

static esp_err_t power_get(httpd_req_t *req)
{
    static const char *const names[] = { "active", "hiz", "off" };
    char json[220];
    // Does the chip still answer on I2C? A chip really in shutdown (PWDN low) must not.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool acks = i2c_master_probe(s_bus, DAC_I2C_ADDR, 30) == ESP_OK;
    xSemaphoreGive(s_lock);
    snprintf(json, sizeof json,
             "{\"state\":\"%s\",\"i2c_ack\":%s,\"muted\":%s,\"hiz_after_s\":%d,\"off_after_s\":%d,\"wakes_from_hiz\":%u,\"wakes_from_off\":%u}\n",
             names[s_state], acks ? "true" : "false", s_muted ? "true" : "false", s_cfg.hiz_s, s_cfg.off_s, (unsigned)s_wakes_hiz, (unsigned)s_wakes_off);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t power_post(httpd_req_t *req)
{
    if (!web_authorized(req)) return web_deny(req);
    char body[80];
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
    int hiz = s_cfg.hiz_s, off = s_cfg.off_s, v;
    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (sscanf(line, "hiz_after_s=%d", &v) == 1) hiz = v;
        else if (sscanf(line, "off_after_s=%d", &v) == 1) off = v;
        else {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "cannot parse a line\n");
        }
    }
    if (!dac_power_set(hiz, off)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "rejected: hiz_after_s 0-3600, off_after_s 0-86400, and off must be later than hiz (0 = never)\n");
    }
    return power_get(req);
}

void dac_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/power", .method = HTTP_GET,  .handler = power_get },
        { .uri = "/power", .method = HTTP_POST, .handler = power_post },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
}
