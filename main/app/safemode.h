// Crash-loop protection. Every boot increments a counter in flash; the counter is cleared once the firmware has
// stayed up for 30 s. If three boots in a row never got that far, safe mode is active for this boot: the optional
// features (network audio receivers, ambient scene, radio, synthesizer) are held off, so the web server and OTA
// still come up and a bad setting can be fixed, instead of the board restarting forever.
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"

#if CONFIG_AB_FEATURE_SAFEMODE
void safemode_boot(void);       // call once at start, after nvs_flash_init
bool safemode_active(void);
void safemode_healthy(void);    // call periodically; clears the counter after 30 s of uptime
#else   // switched off: never in safe mode
static inline void safemode_boot(void) {}
static inline bool safemode_active(void) { return false; }
static inline void safemode_healthy(void) {}
#endif
