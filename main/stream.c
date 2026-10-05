#include "stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "media.h"
#include "ota_http.h"

static const char *TAG = "stream";

#define RECV_CHUNK   4096
#define IDLE_TIMEOUT_S 30

// Reads a line (without the newline) one byte at a time so no audio is consumed with it.
static bool read_line(int sock, char *buf, size_t max)
{
    size_t n = 0;
    while (n + 1 < max) {
        char c;
        int r = recv(sock, &c, 1, 0);
        if (r != 1) return false;
        if (c == '\n') {
            buf[n] = '\0';
            if (n && buf[n - 1] == '\r') buf[n - 1] = '\0';
            return true;
        }
        buf[n++] = c;
    }
    return false;
}

static void serve_client(int sock)
{
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char line[128];
    if (!read_line(sock, line, sizeof line)) return;

    char pass[72] = "";
    unsigned rate = 48000, channels = 2;
    if (sscanf(line, "STREAM %71s %u %u", pass, &rate, &channels) < 1 || rate < 8000 || rate > 192000 ||
        channels < 1 || channels > 2) {
        send(sock, "ERR bad request\n", 16, 0);
        return;
    }
    if (!web_password_ok(pass)) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // slow down guessing
        send(sock, "ERR denied\n", 11, 0);
        return;
    }
    if (!media_begin(MEDIA_STREAM, "network stream")) {
        send(sock, "ERR busy\n", 9, 0);
        return;
    }
    send(sock, "OK\n", 3, 0);
    ESP_LOGI(TAG, "stream started: %u Hz, %u ch", rate, channels);

    tv.tv_sec = 1;  // wake up regularly to notice aborts and stalled senders
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    static EXT_RAM_BSS_ATTR uint8_t raw[RECV_CHUNK + 8];
    static EXT_RAM_BSS_ATTR int16_t stereo[RECV_CHUNK + 16];  // mono input yields up to RECV_CHUNK/2 frames of 2 samples
    static EXT_RAM_BSS_ATTR int16_t out[4096];
    resampler_t rs;
    resampler_init(&rs, rate);
    size_t carry = 0;
    int idle = 0;
    const size_t frame_bytes = 2 * channels;

    while (!media_aborted()) {
        int n = recv(sock, raw + carry, RECV_CHUNK, 0);
        if (n == 0) break;  // client closed: playback ends once the buffer drains
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++idle >= IDLE_TIMEOUT_S) {
                    ESP_LOGW(TAG, "sender stalled, ending the stream");
                    media_abort();
                }
                continue;
            }
            break;
        }
        idle = 0;
        size_t have = carry + n;
        size_t frames = have / frame_bytes;
        const int16_t *src = (const int16_t *)raw;
        for (size_t i = 0; i < frames; i++) {
            stereo[2 * i] = src[channels * i];
            stereo[2 * i + 1] = channels > 1 ? src[channels * i + 1] : src[channels * i];
        }
        carry = have - frames * frame_bytes;
        memmove(raw, raw + frames * frame_bytes, carry);

        size_t produced = frames;
        const int16_t *play = stereo;
        if (rate != 48000) {
            // The output buffer holds 2048 frames; 256 input frames at 8 kHz make 1536 output frames.
            size_t done = 0;
            while (done < frames && !media_aborted()) {
                size_t slice = frames - done > 256 ? 256 : frames - done;
                produced = resampler_process(&rs, stereo + 2 * done, slice, out, 2048);
                if (produced && media_write(out, produced) < produced) break;
                done += slice;
            }
            continue;
        }
        if (media_write(play, produced) < produced) break;
    }
    media_finish();
    ESP_LOGI(TAG, "stream ended (underruns: %u)", (unsigned)media_underruns());
}

static void stream_task(void *arg)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 1) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %d", STREAM_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on port %d", STREAM_PORT);
    while (true) {
        int sock = accept(listener, NULL, NULL);
        if (sock < 0) continue;
        serve_client(sock);
        close(sock);
    }
}

bool stream_start(void)
{
    return xTaskCreate(stream_task, "stream", 6144, NULL, 4, NULL) == pdPASS;
}
