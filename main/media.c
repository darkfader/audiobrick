#include "media.h"

#include <string.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tone.h"

static const char *TAG = "media";

#define RING_BYTES       (256 * 1024)               // power of two, about 1.4 s of 48 kHz stereo
#define FRAME_BYTES      4
#define PREBUFFER_FRAMES 8000                       // about 170 ms before playback starts

static uint8_t *s_ring;
static volatile uint32_t s_head;                    // total bytes written (producer)
static volatile uint32_t s_tail;                    // total bytes read (consumer)
static volatile media_kind_t s_kind;
static volatile bool s_finished, s_abort, s_started;
static volatile uint32_t s_underruns;
static char s_label[40];
static SemaphoreHandle_t s_lock;

bool media_init(void)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_lock) {
        ESP_LOGE(TAG, "no memory for the audio buffer");
        return false;
    }
    return true;
}

bool media_begin(media_kind_t kind, const char *label)
{
    if (!s_ring || tone_get().enabled) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_kind == MEDIA_NONE) {
        s_head = s_tail = 0;
        s_finished = s_abort = s_started = false;
        s_underruns = 0;
        strlcpy(s_label, label ? label : "", sizeof s_label);
        s_kind = kind;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

size_t media_write(const int16_t *stereo, size_t frames)
{
    const uint8_t *src = (const uint8_t *)stereo;
    size_t left = frames * FRAME_BYTES;
    while (left > 0) {
        if (s_abort || s_kind == MEDIA_NONE) break;
        uint32_t used = s_head - s_tail;
        uint32_t space = RING_BYTES - used;
        if (space < FRAME_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t n = left < space ? left : space;
        n -= n % FRAME_BYTES;
        uint32_t pos = s_head & (RING_BYTES - 1);
        size_t first = RING_BYTES - pos;
        if (first > n) first = n;
        memcpy(s_ring + pos, src, first);
        if (n > first) memcpy(s_ring, src + first, n - first);
        __atomic_store_n(&s_head, s_head + (uint32_t)n, __ATOMIC_RELEASE);
        src += n;
        left -= n;
    }
    return frames - left / FRAME_BYTES;
}

void media_finish(void)
{
    s_finished = true;
}

void media_abort(void)
{
    if (s_kind != MEDIA_NONE) s_abort = true;
}

bool media_aborted(void)
{
    return s_abort;
}

bool media_active(void)
{
    return s_kind != MEDIA_NONE;
}

size_t media_read(int16_t *out, size_t frames)
{
    if (s_kind == MEDIA_NONE) return 0;
    if (s_abort) {
        s_tail = s_head;
        s_kind = MEDIA_NONE;
        return 0;
    }
    uint32_t used = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE) - s_tail;
    if (!s_started) {
        if (used >= PREBUFFER_FRAMES * FRAME_BYTES || (s_finished && used > 0)) {
            s_started = true;
        } else {
            if (s_finished) s_kind = MEDIA_NONE;  // ended before anything was played
            return 0;
        }
    }
    size_t want = frames * FRAME_BYTES;
    size_t n = used < want ? used : want;
    n -= n % FRAME_BYTES;
    uint32_t pos = s_tail & (RING_BYTES - 1);
    size_t first = RING_BYTES - pos;
    if (first > n) first = n;
    memcpy(out, s_ring + pos, first);
    if (n > first) memcpy((uint8_t *)out + first, s_ring, n - first);
    __atomic_store_n(&s_tail, s_tail + (uint32_t)n, __ATOMIC_RELEASE);

    if (n < want) {
        if (s_finished) {
            s_kind = MEDIA_NONE;  // buffer drained: end of the session
        } else {
            s_underruns++;
        }
    }
    return n / FRAME_BYTES;
}

media_kind_t media_kind(void)      { return s_kind; }
const char *media_label(void)      { return s_label; }
uint32_t media_underruns(void)     { return s_underruns; }

uint32_t media_buffer_ms(void)
{
    return (s_head - s_tail) / FRAME_BYTES / (SAMPLE_RATE / 1000);
}

// ---- resampler ---------------------------------------------------------------------------

void resampler_init(resampler_t *r, uint32_t in_rate)
{
    r->step = (float)in_rate / SAMPLE_RATE;
    r->pos = 0.0f;
    r->prev[0] = r->prev[1] = 0;
    r->primed = false;
}

size_t resampler_process(resampler_t *r, const int16_t *in, size_t nin, int16_t *out, size_t out_cap)
{
    size_t n = 0;
    for (size_t i = 0; i < nin; i++) {
        int16_t cur_l = in[2 * i], cur_r = in[2 * i + 1];
        if (!r->primed) {
            r->prev[0] = cur_l;
            r->prev[1] = cur_r;
            r->primed = true;
            continue;
        }
        while (r->pos < 1.0f && n < out_cap) {
            out[2 * n]     = (int16_t)(r->prev[0] + (cur_l - r->prev[0]) * r->pos);
            out[2 * n + 1] = (int16_t)(r->prev[1] + (cur_r - r->prev[1]) * r->pos);
            n++;
            r->pos += r->step;
        }
        r->pos -= 1.0f;
        r->prev[0] = cur_l;
        r->prev[1] = cur_r;
    }
    return n;
}
