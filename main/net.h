#pragma once
#include <stdbool.h>

// Brings up the W5500 and DHCP. Returns once the driver is started; the IP arrives later.
bool net_start(void);
bool net_has_ip(void);
// Dotted-quad of the current address, or "" if none.
const char *net_ip_str(void);
