#pragma once
#include <stdbool.h>

// Brings up the W5500 and DHCP. Returns once the driver is started; the IP arrives later.
bool net_start(void);
bool net_has_ip(void);
void *net_eth_handle(void);   // esp_eth_handle_t, for promiscuous mode (multicast audio)
void net_set_promiscuous(bool on);
// Dotted-quad of the current address, or "" if none.
const char *net_ip_str(void);
