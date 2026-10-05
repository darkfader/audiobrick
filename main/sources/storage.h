#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

#define STORAGE_PATH "/storage"

#if CONFIG_AB_FEATURE_CLIPS
// Mounts the FAT partition "storage" (formats it on first use).
bool storage_init(void);
bool storage_info(size_t *total_bytes, size_t *free_bytes);
#else   // clips switched off: inert stand-ins so the rest of the code does not need #if
static inline bool storage_init(void) { return false; }
static inline bool storage_info(size_t *total_bytes, size_t *free_bytes) { if (total_bytes) *total_bytes = 0; if (free_bytes) *free_bytes = 0; return false; }
#endif
