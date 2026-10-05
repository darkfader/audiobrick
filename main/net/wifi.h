#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// Wi-Fi client (station) as a second network interface next to the wired one. Both get an address from DHCP; the wired one wins for outgoing traffic.
//
// The Brick remembers up to 8 networks (name + password, in flash). While Wi-Fi is on it scans every few seconds when it is not connected and joins the
// strongest remembered network it can see; if the link drops it looks again. Adding a network from the page switches Wi-Fi on.
// Wi-Fi is off until it is switched on, by hand (which also allows scanning) or by adding a network. It costs about 40 KB of internal RAM while on. The password is never sent back by any endpoint.
//
//   GET  /wifi                   {"enabled","connected","ssid","ip","rssi","known":[{"ssid","connected"}...]}   (login)
//   POST /wifi?on=0|1            switch Wi-Fi off/on
//   POST /wifi/scan              start a scan;  GET /wifi/scan lists the networks heard: {"scanning","networks":[{"ssid","rssi","secure","known"}...]}
//   POST /wifi/add               body "ssid\npassword" (password line empty or missing for an open network): remember it and switch Wi-Fi on
//   POST /wifi/forget            body = ssid
//   POST /wifi/connect           body = ssid: join that remembered network now

void wifi_client_init(void);                                   // start Wi-Fi if it was left switched on
void wifi_client_http_register(httpd_handle_t server);
