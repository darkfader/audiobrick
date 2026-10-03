#include "safemode.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "safemode";
static bool s_safe;
static bool s_cleared;

static void write_counter(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bootcnt", v);
    nvs_commit(h);
    nvs_close(h);
}

void safemode_boot(void)
{
    uint8_t n = 0;
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bootcnt", &n);
        nvs_close(h);
    }
    s_safe = n >= 3;  // three boots in a row that never stayed up for 30 s
    if (s_safe) ESP_LOGE(TAG, "SAFE MODE: %u failed boots in a row, optional features are off for this boot", n);
    write_counter(n < 250 ? n + 1 : n);
}

bool safemode_active(void) { return s_safe; }

void safemode_healthy(void)
{
    if (s_cleared || esp_timer_get_time() < 30LL * 1000000) return;
    write_counter(0);
    s_cleared = true;
    if (s_safe) ESP_LOGW(TAG, "stayed up for 30 s; the next boot will be normal again");
}
