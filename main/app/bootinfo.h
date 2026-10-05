#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

// Why the board last restarted, and how often. Call after nvs_flash_init() and before anything slow.
#if CONFIG_AB_FEATURE_BOOTINFO
void bootinfo_init(void);
const char *bootinfo_reason(void);   // short text for the page: "power-on", "crash (panic)", "brown-out (supply dipped)", ...
bool bootinfo_abnormal(void);        // true for a crash, a watchdog reset or a brown-out
uint32_t bootinfo_count(void);       // boots since the flash was first used
uint32_t bootinfo_crashes(void);     // of those, abnormal ones
#else   // switched off
static inline void bootinfo_init(void) {}
static inline const char *bootinfo_reason(void) { return "not recorded"; }
static inline bool bootinfo_abnormal(void) { return false; }
static inline uint32_t bootinfo_count(void) { return 0; }
static inline uint32_t bootinfo_crashes(void) { return 0; }
#endif
