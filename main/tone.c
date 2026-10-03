// I2S output task: plays the media buffer (stream or clip) when a session is open, otherwise the
// sine test tone, otherwise silence. The amp is unmuted only while there is signal.
#include "tone.h"

#include <math.h>
#include <stdint.h>
#include "board.h"
#include "dac.h"
#include "driver/i2s_std.h"
#include "eq.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "limits.h"
#include "media.h"

static const char *TAG = "tone";

#define TABLE_BITS 10
#define TABLE_SIZE (1 << TABLE_BITS)
#define FRAMES     256
#define RAMP_MS    100  // fade time for starting/stopping and for tone level changes

static i2s_chan_handle_t s_tx;
static int16_t s_table[TABLE_SIZE + 1];  // one guard entry so interpolation never wraps
static volatile tone_state_t s_state = { false, 440.0f, -24.0f };
static volatile int64_t s_deadline_us;
static bool s_dac_unmuted;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void tone_set(bool enabled, float freq_hz, float level_dbfs)
{
    float lowest = limits_get().min_hz;  // the speaker profile sets the lowest safe frequency
    if (lowest < TONE_MIN_HZ) lowest = TONE_MIN_HZ;
    tone_state_t s = {
        .enabled = enabled,
        .freq_hz = clampf(freq_hz, lowest, TONE_MAX_HZ),
        .level_dbfs = clampf(level_dbfs, TONE_MIN_DBFS, TONE_MAX_DBFS),
    };
    s_state = s;
}

tone_state_t tone_get(void)
{
    return s_state;
}

void tone_hold(int ttl_s)
{
    if (ttl_s < 1) ttl_s = 1;
    if (ttl_s > 3600) ttl_s = 3600;
    s_deadline_us = esp_timer_get_time() + (int64_t)ttl_s * 1000000;
}

static void audio_task(void *arg)
{
    static int16_t buf[FRAMES * 2];
    static int16_t media_buf[FRAMES * 2];
    uint32_t phase = 0;
    float gain = 0.0f;  // current linear gain, ramped toward the target
    bool was_media = false;
    const float ramp_step = 1.0f / (SAMPLE_RATE * RAMP_MS / 1000.0f);  // full scale in RAMP_MS

    while (true) {
        tone_state_t s = s_state;
        if (s.enabled && esp_timer_get_time() > s_deadline_us) {
            s_state.enabled = false;  // nobody refreshed the dead-man timer: stop
            s.enabled = false;
        }

        bool media = media_active();
        size_t have = media ? media_read(media_buf, FRAMES) : 0;
        media = media_active() || have > 0;  // a session that just ended still delivers its last frames
        if (was_media && !media) {
            // The media gain is 1.0 here. Without this reset the idle tone generator would fade out
            // from full scale and play a 100 ms beep at the end of every stream or clip.
            gain = 0.0f;
        }
        was_media = media;

        float target;
        if (media) {
            target = 1.0f;
        } else {
            target = s.enabled ? powf(10.0f, s.level_dbfs / 20.0f) : 0.0f;
        }
        uint32_t inc = (uint32_t)(s.freq_hz / SAMPLE_RATE * 4294967296.0f);

        // Unmute the amp only while there is signal; mute again once faded out.
        bool want_sound = media || s.enabled;
        if (want_sound && !s_dac_unmuted) {
            s_dac_unmuted = dac_set_mute(false);
        } else if (!want_sound && gain == 0.0f && s_dac_unmuted) {
            s_dac_unmuted = !dac_set_mute(true);
        }

        for (int i = 0; i < FRAMES; i++) {
            if (gain < target) {
                gain = fminf(gain + ramp_step, target);
            } else if (gain > target) {
                gain = fmaxf(gain - ramp_step, target);
            }
            if (media) {
                bool ok = (size_t)i < have;
                buf[2 * i]     = ok ? (int16_t)(media_buf[2 * i] * gain) : 0;
                buf[2 * i + 1] = ok ? (int16_t)(media_buf[2 * i + 1] * gain) : 0;
            } else {
                // Linear interpolation between table entries keeps high tones clean.
                uint32_t idx = phase >> (32 - TABLE_BITS);
                int32_t frac = (phase >> (16 - TABLE_BITS)) & 0xFFFF;  // 16-bit position between entries
                int32_t a = s_table[idx], b = s_table[idx + 1];
                int16_t v = (int16_t)((a + (((b - a) * frac) >> 16)) * gain);
                buf[2 * i] = v;
                buf[2 * i + 1] = v;
                phase += inc;
            }
        }
        eq_process(buf, FRAMES);  // speaker EQ for every source
        size_t written;
        i2s_channel_write(s_tx, buf, sizeof buf, &written, portMAX_DELAY);
    }
}

bool tone_init(void)
{
    for (int i = 0; i <= TABLE_SIZE; i++) {
        s_table[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * i / TABLE_SIZE));
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = FRAMES;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return false;
    }
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 6, NULL, 1);
    return true;
}
