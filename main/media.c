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

#define FRAME_BYTES      4
#define PREBUFFER_FRAMES 8000  // about 170 ms before playback starts

// Ring sizes are powers of two: the main slot holds about 1.4 s, the ambient slots about 0.7 s.
static const uint32_t RING_BYTES[MEDIA_SLOTS] = { 256 * 1024, 128 * 1024, 128 * 1024 };

typedef struct {
    uint8_t *ring;
    volatile uint32_t head;  // total bytes written (producer)
    volatile uint32_t tail;  // total bytes read (consumer)
    volatile media_kind_t kind;
    volatile bool finished, abort, started, in_underrun;
    volatile uint32_t underruns;
    char label[40];
} slot_t;

static slot_t s_slot[MEDIA_SLOTS];
static SemaphoreHandle_t s_lock;

bool media_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
    for (int i = 0; i < MEDIA_SLOTS; i++) {
        s_slot[i].ring = heap_caps_malloc(RING_BYTES[i], MALLOC_CAP_SPIRAM);
        if (!s_slot[i].ring) {
            ESP_LOGE(TAG, "no memory for the audio buffer of slot %d", i);
            return false;
        }
    }
    return true;
}

bool media_begin_slot(int slot, media_kind_t kind, const char *label)
{
    if (slot < 0 || slot >= MEDIA_SLOTS) return false;
    slot_t *s = &s_slot[slot];
    if (!s->ring || tone_get().enabled) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s->kind == MEDIA_NONE) {
        s->head = s->tail = 0;
        s->finished = s->abort = s->started = s->in_underrun = false;
        s->underruns = 0;
        strlcpy(s->label, label ? label : "", sizeof s->label);
        if (slot == SLOT_MAIN) media_set_paused(false);  // a new main session always starts playing
        s->kind = kind;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

size_t media_write_slot(int slot, const int16_t *stereo, size_t frames)
{
    slot_t *s = &s_slot[slot];
    const uint32_t size = RING_BYTES[slot];
    const uint8_t *src = (const uint8_t *)stereo;
    size_t left = frames * FRAME_BYTES;
    while (left > 0) {
        if (s->abort || s->kind == MEDIA_NONE) break;
        uint32_t used = s->head - s->tail;
        uint32_t space = size - used;
        if (space < FRAME_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t n = left < space ? left : space;
        n -= n % FRAME_BYTES;
        uint32_t pos = s->head & (size - 1);
        size_t first = size - pos;
        if (first > n) first = n;
        memcpy(s->ring + pos, src, first);
        if (n > first) memcpy(s->ring, src + first, n - first);
        __atomic_store_n(&s->head, s->head + (uint32_t)n, __ATOMIC_RELEASE);
        src += n;
        left -= n;
    }
    return frames - left / FRAME_BYTES;
}

size_t media_write_nb_slot(int slot, const int16_t *stereo, size_t frames)
{
    slot_t *s = &s_slot[slot];
    const uint32_t size = RING_BYTES[slot];
    if (s->abort || s->kind == MEDIA_NONE) return 0;
    uint32_t space = size - (s->head - s->tail);
    size_t n = frames * FRAME_BYTES;
    if (n > space) n = space;
    n -= n % FRAME_BYTES;
    if (n == 0) return 0;
    uint32_t pos = s->head & (size - 1);
    size_t first = size - pos;
    if (first > n) first = n;
    memcpy(s->ring + pos, stereo, first);
    if (n > first) memcpy(s->ring, (const uint8_t *)stereo + first, n - first);
    __atomic_store_n(&s->head, s->head + (uint32_t)n, __ATOMIC_RELEASE);
    return n / FRAME_BYTES;
}

void media_finish_slot(int slot) { s_slot[slot].finished = true; }

void media_abort_slot(int slot)
{
    if (s_slot[slot].kind != MEDIA_NONE) s_slot[slot].abort = true;
}

bool media_aborted_slot(int slot) { return s_slot[slot].abort; }
bool media_active_slot(int slot) { return s_slot[slot].kind != MEDIA_NONE; }

size_t media_read_slot(int slot, int16_t *out, size_t frames)
{
    slot_t *s = &s_slot[slot];
    const uint32_t size = RING_BYTES[slot];
    if (s->kind == MEDIA_NONE) return 0;
    if (s->abort) {
        s->tail = s->head;
        s->kind = MEDIA_NONE;
        return 0;
    }
    uint32_t used = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) - s->tail;
    if (!s->started) {
        if (used >= PREBUFFER_FRAMES * FRAME_BYTES || (s->finished && used > 0)) {
            s->started = true;
        } else {
            if (s->finished) s->kind = MEDIA_NONE;  // ended before anything was played
            return 0;
        }
    }
    size_t want = frames * FRAME_BYTES;
    size_t n = used < want ? used : want;
    n -= n % FRAME_BYTES;
    uint32_t pos = s->tail & (size - 1);
    size_t first = size - pos;
    if (first > n) first = n;
    memcpy(out, s->ring + pos, first);
    if (n > first) memcpy((uint8_t *)out + first, s->ring, n - first);
    __atomic_store_n(&s->tail, s->tail + (uint32_t)n, __ATOMIC_RELEASE);

    if (n < want) {
        if (s->finished) {
            s->kind = MEDIA_NONE;  // buffer drained: end of the session
        } else if (!s->in_underrun) {
            s->underruns++;        // count each dropout once, not every empty block while it lasts
            s->in_underrun = true;
        }
    } else {
        s->in_underrun = false;
    }
    return n / FRAME_BYTES;
}

media_kind_t media_kind_slot(int slot) { return s_slot[slot].kind; }
const char *media_label_slot(int slot) { return s_slot[slot].label; }
uint32_t media_underruns_slot(int slot) { return s_slot[slot].underruns; }

uint32_t media_buffer_ms_slot(int slot)
{
    return (s_slot[slot].head - s_slot[slot].tail) / FRAME_BYTES / (SAMPLE_RATE / 1000);
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
