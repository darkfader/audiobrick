#include "clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media.h"
#include "storage.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

static const char *TAG = "clip";

#define IN_BUF     8192
#define OUT_FRAMES 2048

typedef struct {
    char path[64];
    bool is_wav;
    bool loop;
    int slot;
} clip_job_t;

// Per-playback working state (one per clip task, so several clips can play at once).
typedef struct {
    int slot;
    resampler_t rs;
    uint32_t cur_rate;
    int16_t *stereo;  // 256 frames of interleaved stereo
    int16_t *out;     // OUT_FRAMES of resampled stereo
} ctx_t;

bool clip_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || n > 31) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    if (strstr(name, "..")) return false;
    const char *ext = name + n - 4;
    return strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0;
}

// Sends decoded interleaved PCM (1 or 2 channels at rate Hz) into the player slot. False once aborted.
static bool push_pcm(ctx_t *c, const int16_t *pcm, size_t frames, int channels, uint32_t rate)
{
    if (rate != c->cur_rate) {
        resampler_init(&c->rs, rate);
        c->cur_rate = rate;
    }
    while (frames > 0) {
        size_t n = frames < 256 ? frames : 256;  // 256 frames at 8 kHz resample to 1536 (< OUT_FRAMES)
        for (size_t i = 0; i < n; i++) {
            c->stereo[2 * i] = pcm[channels * i];
            c->stereo[2 * i + 1] = channels > 1 ? pcm[channels * i + 1] : pcm[channels * i];
        }
        size_t produced;
        if (rate == 48000) {
            produced = n;
            memcpy(c->out, c->stereo, n * 4);
        } else {
            produced = resampler_process(&c->rs, c->stereo, n, c->out, OUT_FRAMES);
        }
        if (produced && media_write_slot(c->slot, c->out, produced) < produced) return false;  // aborted
        pcm += channels * n;
        frames -= n;
    }
    return true;
}

// MP3 files start with encoder delay (silence) and end with padding. The first frame is often a Xing/Info
// metadata frame that decodes as silence, and its LAME tag says how many samples to trim at each end.
// Without this trim a looped clip has a 25-35 ms gap at the join.
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Looks for a Xing/Info header in the first frame. Returns true if found.
static bool parse_xing(const uint8_t *fr, int len, uint32_t *frames, int *delay, int *padding)
{
    static const int offsets[] = { 36, 21, 13 };  // MPEG1 stereo, MPEG1 mono / MPEG2 stereo, MPEG2 mono
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        int off = offsets[i];
        if (off + 8 > len) continue;
        if (memcmp(fr + off, "Info", 4) != 0 && memcmp(fr + off, "Xing", 4) != 0) continue;
        uint32_t flags = be32(fr + off + 4);
        const uint8_t *p = fr + off + 8;
        *frames = 0;
        if (flags & 1) { *frames = be32(p); p += 4; }
        if (flags & 2) p += 4;
        if (flags & 4) p += 100;
        if (flags & 8) p += 4;
        *delay = 576;
        *padding = 0;
        if ((p - fr) + 24 <= len) {  // encoder string (9 bytes) ... delay and padding, 12 bits each, at +21
            int d = (p[21] << 4) | (p[22] >> 4);
            int pad = ((p[22] & 0x0F) << 8) | p[23];
            if (d <= 2000 && pad <= 4000) {
                *delay = d;
                *padding = pad;
            }
        }
        return true;
    }
    return false;
}

static void play_mp3(FILE *f, ctx_t *c)
{
    mp3dec_t *dec = calloc(1, sizeof *dec);
    uint8_t *in = malloc(IN_BUF);
    int16_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!dec || !in || !pcm) {
        ESP_LOGE(TAG, "out of memory");
        goto done;
    }
    mp3dec_init(dec);

    // Skip an ID3v2 tag at the start of the file, if there is one.
    uint8_t id3[10];
    long start = 0;
    if (fread(id3, 1, 10, f) == 10 && !memcmp(id3, "ID3", 3)) {
        start = 10 + (((id3[6] & 0x7F) << 21) | ((id3[7] & 0x7F) << 14) | ((id3[8] & 0x7F) << 7) | (id3[9] & 0x7F));
    }
    fseek(f, start, SEEK_SET);

    size_t in_len = 0;
    bool eof = false;
    bool first = true;
    int64_t skip = 1105;          // default when there is no gapless tag: the usual LAME delay plus decoder delay
    int64_t valid = INT64_MAX;    // samples per channel that belong to the audio
    uint32_t xing_frames = 0;
    int xing_padding = 0;
    int64_t pos = 0;              // decoded samples per channel so far
    while (!media_aborted_slot(c->slot)) {
        if (!eof && in_len < IN_BUF) {
            size_t got = fread(in + in_len, 1, IN_BUF - in_len, f);
            if (got == 0) eof = true;
            in_len += got;
        }
        if (in_len == 0) break;
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, in, (int)in_len, pcm, &info);
        if (info.frame_bytes == 0) {  // no frame found in the remaining bytes
            if (eof) break;
            in_len = 0;
            continue;
        }
        if (first) {
            first = false;
            uint32_t frames;
            int delay, padding;
            int off = info.frame_offset > 0 ? info.frame_offset : 0;
            if (parse_xing(in + off, (int)in_len - off, &frames, &delay, &padding)) {
                skip = delay + 529;  // encoder delay plus the 528+1 samples of decoder delay
                xing_frames = frames;
                xing_padding = padding;
                memmove(in, in + info.frame_bytes, in_len - info.frame_bytes);
                in_len -= info.frame_bytes;
                continue;  // this frame is metadata, not audio
            }
        }
        if (xing_frames && valid == INT64_MAX) {
            int spf = info.hz >= 32000 ? 1152 : 576;
            int64_t total = (int64_t)xing_frames * spf;
            valid = total - (skip - 529) - xing_padding;
            if (valid < 0) valid = INT64_MAX;
        }
        memmove(in, in + info.frame_bytes, in_len - info.frame_bytes);
        in_len -= info.frame_bytes;
        if (samples > 0) {
            int64_t a = skip > pos ? skip - pos : 0;                 // first sample to play within this frame
            int64_t b = samples;
            if (valid != INT64_MAX && skip + valid - pos < b) b = skip + valid - pos;
            pos += samples;
            if (b > a && !push_pcm(c, pcm + a * info.channels, (size_t)(b - a), info.channels, info.hz)) break;
            if (valid != INT64_MAX && pos >= skip + valid) break;  // the rest is encoder padding
        }
    }
done:
    free(dec);
    free(in);
    free(pcm);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void play_wav(FILE *f, ctx_t *c)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        ESP_LOGE(TAG, "not a WAV file");
        return;
    }
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, f) == 8) {
        uint32_t size = rd32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4) && size >= 16) {
            uint8_t b[16];
            if (fread(b, 1, 16, f) != 16) return;
            fmt = rd16(b);
            channels = rd16(b + 2);
            rate = rd32(b + 4);
            bits = rd16(b + 14);
            fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
        } else if (!memcmp(chunk, "data", 4)) {
            if (fmt != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 192000) {
                ESP_LOGE(TAG, "unsupported WAV (need 16-bit PCM, mono/stereo): fmt=%u bits=%u ch=%u rate=%u",
                         fmt, bits, channels, (unsigned)rate);
                return;
            }
            int16_t *pcm = malloc(1024 * channels * sizeof(int16_t));
            if (!pcm) return;
            uint32_t left = size;
            while (left > 0 && !media_aborted_slot(c->slot)) {
                size_t want = left < 1024 * channels * 2 ? left : 1024 * channels * 2;
                size_t got = fread(pcm, 1, want, f);
                if (got == 0) break;
                left -= got;
                size_t frames = got / (2 * channels);
                if (!push_pcm(c, pcm, frames, channels, rate)) break;
            }
            free(pcm);
            return;
        } else {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }
}

static void clip_task(void *arg)
{
    clip_job_t *job = arg;
    FILE *f = fopen(job->path, "rb");
    ctx_t c = { .slot = job->slot };
    c.stereo = malloc(256 * 2 * sizeof(int16_t));
    c.out = malloc(OUT_FRAMES * 2 * sizeof(int16_t));
    if (f && c.stereo && c.out) {
        do {
            int64_t started = esp_timer_get_time();
            if (job->is_wav) play_wav(f, &c);
            else play_mp3(f, &c);
            if (esp_timer_get_time() - started < 200000) break;  // empty or broken file: do not spin
            fseek(f, 0, SEEK_SET);
        } while (job->loop && !media_aborted_slot(job->slot));
    } else {
        ESP_LOGE(TAG, "cannot open %s", job->path);
    }
    if (f) fclose(f);
    free(c.stereo);
    free(c.out);
    int slot = job->slot;
    free(job);
    media_finish_slot(slot);
    vTaskDelete(NULL);
}

bool clip_play_slot(const char *name, bool loop, int slot)
{
    if (!clip_name_valid(name)) return false;
    clip_job_t *job = calloc(1, sizeof *job);
    if (!job) return false;
    snprintf(job->path, sizeof job->path, STORAGE_PATH "/%s", name);
    job->is_wav = strcasecmp(name + strlen(name) - 4, ".wav") == 0;
    job->loop = loop;
    job->slot = slot;

    FILE *f = fopen(job->path, "rb");  // fail early with a proper error if the file is missing
    if (!f) {
        free(job);
        return false;
    }
    fclose(f);

    if (!media_begin_slot(slot, MEDIA_CLIP, name)) {
        free(job);
        return false;
    }
    // minimp3 keeps about 18 KB of scratch data on the stack while decoding a frame.
    if (xTaskCreate(clip_task, "clip", 28672, job, 4, NULL) != pdPASS) {
        media_abort_slot(slot);
        media_finish_slot(slot);
        free(job);
        return false;
    }
    return true;
}

bool clip_play(const char *name, bool loop)
{
    return clip_play_slot(name, loop, SLOT_MAIN);
}
