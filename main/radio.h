// Internet radio: downloads an MP3 stream over HTTP or HTTPS and plays it on the main channel.
// AAC, Ogg and HLS stations are not supported (the decoder is MP3 only); the board says so instead of playing noise.
#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include "esp_http_server.h"

#define RADIO_PRESETS 8

typedef struct {
    char name[24];
    char url[160];
} radio_preset_t;

#if CONFIG_AB_FEATURE_RADIO
void radio_init(void);                          // loads the station list
bool radio_play(const char *url, const char *name);  // false if the URL is invalid or the main channel cannot be taken
bool radio_play_preset(int index);
bool radio_running(void);
const char *radio_status(void);                 // "connecting", "playing", "reconnecting", or an error text
void radio_http_register(httpd_handle_t server);
#else  // not built
static inline void radio_init(void) {}
#endif
