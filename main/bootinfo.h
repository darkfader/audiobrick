#pragma once
#include <stdbool.h>
#include <stdint.h>

// Why the board last restarted, and how often. Call after nvs_flash_init() and before anything slow.
void bootinfo_init(void);
const char *bootinfo_reason(void);   // short text for the page: "power-on", "crash (panic)", "brown-out (supply dipped)", ...
bool bootinfo_abnormal(void);        // true for a crash, a watchdog reset or a brown-out
uint32_t bootinfo_count(void);       // boots since the flash was first used
uint32_t bootinfo_crashes(void);     // of those, abnormal ones
