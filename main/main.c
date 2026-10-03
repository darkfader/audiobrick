// Esparagus Audio Brick (ESP32): Ethernet OTA + sine test tone.
// Status LED: blue = no IP yet, green = online, red = amp fault.
#include "board.h"
#include "dac.h"
#include "eq.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "limits.h"
#include "media.h"
#include "net.h"
#include "storage.h"
#include "stream.h"
#include "nvs_flash.h"
#include "ota_http.h"
#include "tone.h"

static const char *TAG = "audiobrick";

static void set_led(led_strip_handle_t strip, uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(strip, 0, r, g, b);
    led_strip_refresh(strip);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    limits_init();
    eq_init();

    led_strip_config_t strip_cfg = { .strip_gpio_num = PIN_LED, .max_leds = 1 };
    led_strip_rmt_config_t rmt_cfg = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_handle_t strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));
    set_led(strip, 0, 0, 16);

    // I2S clocks must run before the amp is configured; the output stays silent and muted.
    if (!media_init() || !tone_init() || !dac_init()) {
        ESP_LOGE(TAG, "audio init failed");
    }
    storage_init();  // clips; fails harmlessly on boards that still have the old partition table

    if (!net_start() || !ota_http_start() || !stream_start()) {
        ESP_LOGE(TAG, "network init failed");
    }

    bool marked_valid = false;
    int online_ticks = 0;
    while (true) {
        bool online = net_has_ip();
        if (dac_fault_active()) {
            tone_set(false, tone_get().freq_hz, tone_get().level_dbfs);
            set_led(strip, 32, 0, 0);
        } else {
            set_led(strip, 0, online ? 16 : 0, online ? 0 : 16);
        }
        // Keep the new image only after it has been online for a few seconds.
        online_ticks = online ? online_ticks + 1 : 0;
        if (!marked_valid && online_ticks >= 5) {
            ota_mark_valid();
            marked_valid = true;
            ESP_LOGI(TAG, "image marked valid; http://%s/status", net_ip_str());
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
