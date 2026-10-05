#include "sdkconfig.h"
#include "bootinfo.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

#if CONFIG_AB_FEATURE_BOOTINFO  // AB_GATE: the whole file is only built when this feature is switched on


static const char *TAG = "bootinfo";
static const char *s_reason = "unknown";
static bool s_abnormal;
static uint32_t s_count, s_crashes;

void bootinfo_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    switch (r) {
    case ESP_RST_POWERON:   s_reason = "power-on"; break;
    case ESP_RST_EXT:       s_reason = "reset button"; break;
    case ESP_RST_SW:        s_reason = "software restart (update or reboot)"; break;
    case ESP_RST_DEEPSLEEP: s_reason = "wake from deep sleep"; break;
    case ESP_RST_PANIC:     s_reason = "crash (panic)"; s_abnormal = true; break;
    case ESP_RST_INT_WDT:   s_reason = "crash (interrupt watchdog)"; s_abnormal = true; break;
    case ESP_RST_TASK_WDT:  s_reason = "crash (task watchdog)"; s_abnormal = true; break;
    case ESP_RST_WDT:       s_reason = "crash (watchdog)"; s_abnormal = true; break;
    case ESP_RST_BROWNOUT:  s_reason = "brown-out (supply dipped)"; s_abnormal = true; break;
    default:                s_reason = "unknown"; break;
    }
    nvs_handle_t h;
    if (nvs_open("audiobrick", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boots", &s_count);
        nvs_get_u32(h, "crashes", &s_crashes);
        s_count++;
        if (s_abnormal) s_crashes++;
        nvs_set_u32(h, "boots", s_count);
        nvs_set_u32(h, "crashes", s_crashes);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "boot #%u, last reset: %s (%u abnormal so far)", (unsigned)s_count, s_reason, (unsigned)s_crashes);
}

const char *bootinfo_reason(void) { return s_reason; }
bool bootinfo_abnormal(void) { return s_abnormal; }
uint32_t bootinfo_count(void) { return s_count; }
uint32_t bootinfo_crashes(void) { return s_crashes; }

#endif  // CONFIG_AB_FEATURE_BOOTINFO
