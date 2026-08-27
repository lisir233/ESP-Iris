#include "iris_example.h"

#include <stdio.h>

#include "esp_iris.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#define FILE_MOUNT_PATH "/files"
#define FILE_VOLUME_ID  "files"

static const char *TAG = "iris_file_transfer";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static void seed_example_file(void)
{
    FILE *file = fopen(FILE_MOUNT_PATH "/README.txt", "rb");
    if (file != NULL) {
        (void)fclose(file);
        return;
    }

    static const char contents[] =
        "ESP-Iris file transfer example\n"
        "Upload, download, rename, and delete files in this volume.\n";
    file = fopen(FILE_MOUNT_PATH "/README.txt", "wb");
    if (file == NULL) {
        ESP_LOGW(TAG, "Unable to create README.txt");
        return;
    }
    if (fwrite(contents, 1, sizeof(contents) - 1U, file) !=
        sizeof(contents) - 1U) {
        ESP_LOGW(TAG, "Unable to write README.txt");
    }
    if (fclose(file) != 0) {
        ESP_LOGW(TAG, "Unable to close README.txt");
    }
}

static void mount_storage(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(
        FILE_MOUNT_PATH, "storage", &mount_config, &s_wl_handle));
    seed_example_file();
}

static void register_file_volume(void)
{
    const esp_iris_file_volume_config_t volume = {
        .id = FILE_VOLUME_ID,
        .base_path = FILE_MOUNT_PATH,
        .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                        ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME |
                        ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE |
                        ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME,
    };
    ESP_ERROR_CHECK(esp_iris_file_volume_register(&volume));
}

void iris_example_start(void)
{
    mount_storage();
    register_file_volume();
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI(TAG, "Ready: volume=%s base=%s", FILE_VOLUME_ID,
             FILE_MOUNT_PATH);
}
