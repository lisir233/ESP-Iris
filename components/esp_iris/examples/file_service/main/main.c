#include "esp_iris.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#define FILE_MOUNT_PATH "/files"

static const char *TAG = "iris_file_example";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static void seed_volume(void)
{
    FILE *file = fopen(FILE_MOUNT_PATH "/README.txt", "rb");
    if (file == NULL) {
        file = fopen(FILE_MOUNT_PATH "/README.txt", "wb");
        if (file != NULL) {
            (void)fputs("ESP-Iris file-service example\n", file);
        }
    }
    if (file != NULL) {
        (void)fclose(file);
    } else {
        ESP_LOGW(TAG, "unable to create the seed file");
    }
}

static esp_iris_file_volume_config_t volume_config(const char *id)
{
    return (esp_iris_file_volume_config_t) {
        .id = id,
        .base_path = FILE_MOUNT_PATH,
        .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                        ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME |
                        ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE |
                        ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME,
    };
}

void app_main(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(
        FILE_MOUNT_PATH, "storage", &mount_config, &s_wl_handle));
    seed_volume();

    /* Exercise the stopped-state unregister contract without exposing the
     * probe volume to a host session. */
    const esp_iris_file_volume_config_t probe = volume_config("probe");
    ESP_ERROR_CHECK(esp_iris_file_volume_register(&probe));
    ESP_ERROR_CHECK(esp_iris_file_volume_unregister("probe"));

    const esp_iris_file_volume_config_t volume = volume_config("fs");
    ESP_ERROR_CHECK(esp_iris_file_volume_register(&volume));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI(TAG, "ready: volume=fs base=%s atomic_replace=0",
             FILE_MOUNT_PATH);
}
