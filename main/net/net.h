#pragma once
#include <stdbool.h>

// Brings up the W5500 and DHCP. Returns once the driver is started; the IP arrives later.
bool net_start(void);
bool net_has_ip(void);
void *net_eth_handle(void);   // esp_eth_handle_t, for promiscuous mode (multicast audio)
void net_set_promiscuous(bool on);
// Dotted-quad of the current address (wired if it has one, otherwise Wi-Fi), or "" if none. net_has_ip() is true when either interface has one.
const char *net_ip_str(void);
const char *net_eth_ip_str(void);
const char *net_wifi_ip_str(void);
