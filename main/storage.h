#pragma once
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_PATH "/storage"

// Mounts the FAT partition "storage" (formats it on first use).
bool storage_init(void);
bool storage_info(size_t *total_bytes, size_t *free_bytes);
