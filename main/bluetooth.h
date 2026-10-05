#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// Bluetooth audio (A2DP sink): a phone or PC can play music on the Brick. Classic Bluetooth, so ESP32 only (not the S3).
//
// Safety: the Brick is only connectable by devices it has bonded with. New devices can pair only while the pairing window is open
// (opened from the web page, closes by itself after 2 minutes); pairing requests outside the window are refused.
// Pairing method: by default the legacy PIN 0000, accepted only while the pairing window is open. The newer "Secure Simple Pairing" ("just works",
// switch it on with POST /bluetooth?ssp=1) is not used by default because the Bluetooth stack then demands man-in-the-middle protection from the audio
// service, which a screen-less speaker cannot provide: it re-authenticates every incoming audio connection, and a Windows PC's Realtek adapter never answers
// that, so the audio link never opens (see docs/bluetooth.md).
// "Off" (setting) means: invisible and refusing connections; the Bluetooth stack itself is only unloaded by a restart.

void bluetooth_init(void);                         // start the stack if it was left switched on (flash setting)
void bluetooth_http_register(httpd_handle_t server);   // GET/POST /bluetooth, POST /bluetooth/pair, /bluetooth/forget
