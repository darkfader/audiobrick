// Network audio receivers that let a PC send its sound to the board without extra software on the board's side:
//   VBAN   (VB-Audio, built into Voicemeeter): UDP port 6980, 28-byte header + PCM
//   Scream (open-source virtual sound card for Windows): UDP port 4010, 5-byte header + PCM; unicast to the
//          board, or multicast 239.255.77.77
// Both play on the main channel, are capped by the speaker profile's volume limit, go through the EQ, and are
// off until switched on. Neither protocol has a password, so an optional "allowed sender" address is provided.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#define VBAN_PORT   6980
#define SCREAM_PORT 4010

typedef struct {
    bool vban_on;
    bool scream_on;
    bool scream_multicast;   // also join 239.255.77.77 (needs the Ethernet chip's multicast filter off)
    char allow_ip[16];       // dotted quad; "" accepts any sender
    char vban_name[17];      // stream name to accept; "" accepts any
} netaudio_cfg_t;

typedef struct {
    uint32_t packets, dropped;
    bool active;
    uint32_t rate;
    int channels;
    char source[20];         // sender address of the current stream
    char name[17];           // VBAN stream name
} netaudio_stat_t;

void netaudio_init(void);                      // loads the settings and starts both receiver tasks
netaudio_cfg_t netaudio_get(void);
bool netaudio_set(const netaudio_cfg_t *cfg);  // validates and stores
netaudio_stat_t netaudio_vban_stat(void);
netaudio_stat_t netaudio_scream_stat(void);
void netaudio_http_register(httpd_handle_t server);
