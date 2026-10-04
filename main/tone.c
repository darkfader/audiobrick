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
#include "speaker_limits.h"
#include "media.h"
#include "synth.h"

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

static volatile float s_duck_gain = 0.25f;  // main channel gain while a clip is mixed over it (default -12 dB)
static int s_duck_db = 12;

void tone_set_duck_db(int db)
{
    if (db < 0) db = 0;
    if (db > 30) db = 30;
    s_duck_db = db;
    s_duck_gain = db == 0 ? 1.0f : powf(10.0f, -(float)db / 20.0f);
}
int tone_duck_db(void) { return s_duck_db; }

void tone_hold(int ttl_s)
{
    if (ttl_s < 1) ttl_s = 1;
    if (ttl_s > 3600) ttl_s = 3600;
    s_deadline_us = esp_timer_get_time() + (int64_t)ttl_s * 1000000;
}

static volatile bool s_main_paused;
static volatile float s_slot_gain[MEDIA_SLOTS] = { 1.0f, 1.0f, 1.0f };

void media_set_paused(bool p) { s_main_paused = p; }
bool media_paused(void) { return s_main_paused; }
void media_set_slot_gain_db(int slot, float db)
{
    if (slot < 0 || slot >= MEDIA_SLOTS) return;
    if (db > 0) db = 0;
    if (db < -40) db = -40;
    s_slot_gain[slot] = powf(10.0f, db / 20.0f);
}

static void audio_task(void *arg)
{
    static int16_t buf[FRAMES * 2];
    static int16_t mbuf[MEDIA_SLOTS][FRAMES * 2];
    uint32_t phase = 0;
    float tone_gain = 0.0f;                 // ramped like the media gains, so tone changes never click
    float g[MEDIA_SLOTS] = { 0 };
    const float ramp_step = 1.0f / (SAMPLE_RATE * RAMP_MS / 1000.0f);  // full scale in RAMP_MS
    // When the last source ends mid-signal (a stopped clip, a cut stream) the output must not jump to zero.
    // It decays smoothly instead (time constant 0.12 s, inaudible after about 1.3 s) and then sits at exactly 0.
    const float decay = expf(-1.0f / (SAMPLE_RATE * 0.12f));
    float last_l = 0.0f, last_r = 0.0f;
    int idle_blocks = 0;  // consecutive silent blocks (256 frames each, about 5.3 ms)

    while (true) {
        tone_state_t s = s_state;
        if (s.enabled && esp_timer_get_time() > s_deadline_us) {
            s_state.enabled = false;  // nobody refreshed the dead-man timer: stop
            s.enabled = false;
        }

        // Read each channel. A paused main channel keeps reading only while it fades out.
        size_t have[MEDIA_SLOTS] = { 0 };
        float target[MEDIA_SLOTS] = { 0 };
        int active_slots = 0;
        for (int i = 0; i < MEDIA_SLOTS; i++) {
            bool active = media_active_slot(i);
            bool want = active && !(i == SLOT_MAIN && s_main_paused);
            if (active && (want || g[i] > 0.0f)) have[i] = media_read_slot(i, mbuf[i], FRAMES);
            if (!media_active_slot(i) && have[i] == 0) g[i] = 0.0f;  // a finished channel must not leave gain behind
            else if (want) active_slots++;
            target[i] = want ? s_slot_gain[i] : 0.0f;
        }
        if (s_duck_gain < 1.0f && media_active_slot(SLOT_MAIN) && media_active_slot(SLOT_EVENT)) target[SLOT_MAIN] *= s_duck_gain;
        // The OSC synthesizer renders into these buffers; it reports whether anything is sounding.
        static float synth_l[FRAMES], synth_r[FRAMES];
        bool synth_on = synth_render(synth_l, synth_r, FRAMES);
        float tone_target = (s.enabled && !media_active_slot(SLOT_MAIN)) ? powf(10.0f, s.level_dbfs / 20.0f) : 0.0f;
        float mix_scale = active_slots > 1 ? 0.75f : 1.0f;  // headroom when two channels play together
        uint32_t inc = (uint32_t)(s.freq_hz / SAMPLE_RATE * 4294967296.0f);

        // Unmute the amp only while there is signal; mute again once everything has faded out.
        bool any_gain = tone_gain > 0.0f;
        for (int i = 0; i < MEDIA_SLOTS; i++) any_gain = any_gain || g[i] > 0.0f || target[i] > 0.0f;
        bool tail = fabsf(last_l) >= 0.5f || fabsf(last_r) >= 0.5f;  // still decaying toward 0
        bool want_sound = any_gain || s.enabled || tail || synth_on || synth_busy();
        if (want_sound) idle_blocks = 0;
        else if (idle_blocks < 100000) idle_blocks++;
        if (want_sound && !s_dac_unmuted) {
            s_dac_unmuted = dac_set_mute(false);
        } else if (!want_sound && s_dac_unmuted && idle_blocks >= 190) {  // about 1 s of digital silence first
            s_dac_unmuted = !dac_set_mute(true);
        }

        for (int n = 0; n < FRAMES; n++) {
            float l = 0.0f, r = 0.0f;
            bool contributed = false;
            for (int i = 0; i < MEDIA_SLOTS; i++) {
                if (g[i] < target[i]) g[i] = fminf(g[i] + ramp_step, target[i]);
                else if (g[i] > target[i]) g[i] = fmaxf(g[i] - ramp_step, target[i]);
                if ((size_t)n < have[i]) {
                    l += mbuf[i][2 * n] * g[i] * mix_scale;
                    r += mbuf[i][2 * n + 1] * g[i] * mix_scale;
                    contributed = true;
                }
            }
            if (tone_gain > 0.0f || tone_target > 0.0f) contributed = true;
            if (synth_on) {
                l += synth_l[n];
                r += synth_r[n];
                contributed = true;
            }
            if (tone_gain < tone_target) tone_gain = fminf(tone_gain + ramp_step, tone_target);
            else if (tone_gain > tone_target) tone_gain = fmaxf(tone_gain - ramp_step, tone_target);
            if (tone_gain > 0.0f) {
                // Linear interpolation between table entries keeps high tones clean.
                uint32_t idx = phase >> (32 - TABLE_BITS);
                int32_t frac = (phase >> (16 - TABLE_BITS)) & 0xFFFF;  // 16-bit position between entries
                int32_t a = s_table[idx], b = s_table[idx + 1];
                float v = (a + (((b - a) * frac) >> 16)) * tone_gain;
                l += v;
                r += v;
            }
            phase += inc;
            if (contributed) {
                last_l = l;
                last_r = r;
            } else {  // nothing is playing: let the last value fade to 0 instead of stepping to it
                last_l = fabsf(last_l) < 0.5f ? 0.0f : last_l * decay;
                last_r = fabsf(last_r) < 0.5f ? 0.0f : last_r * decay;
                l = last_l;
                r = last_r;
            }
            if (l > 32767.0f) l = 32767.0f;
            if (l < -32768.0f) l = -32768.0f;
            if (r > 32767.0f) r = 32767.0f;
            if (r < -32768.0f) r = -32768.0f;
            buf[2 * n] = (int16_t)l;
            buf[2 * n + 1] = (int16_t)r;
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
