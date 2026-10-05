#include "sdkconfig.h"
#include "storage.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#if CONFIG_AB_FEATURE_CLIPS  // AB_GATE: the whole file is only built when this feature is switched on


static const char *TAG = "storage";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

bool storage_init(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,  // a brand-new partition is empty
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_PATH, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (is the partition table flashed?)", esp_err_to_name(err));
        return false;
    }
    size_t total, free_b;
    if (storage_info(&total, &free_b)) {
        ESP_LOGI(TAG, "mounted %s: %u KB total, %u KB free", STORAGE_PATH, (unsigned)(total / 1024), (unsigned)(free_b / 1024));
    }
    return true;
}

bool storage_info(size_t *total_bytes, size_t *free_bytes)
{
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(STORAGE_PATH, &total, &free_b) != ESP_OK) return false;
    *total_bytes = (size_t)total;
    *free_bytes = (size_t)free_b;
    return true;
}

#endif  // CONFIG_AB_FEATURE_CLIPS
