#include "esp_iris.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"

#define FILE_MOUNT_PATH "/files"

static const char *TAG = "iris_services_usb";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static void register_file_volume(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(
        FILE_MOUNT_PATH, "storage", &mount_config, &s_wl_handle));

    FILE *seed = fopen(FILE_MOUNT_PATH "/README.txt", "rb");
    if (seed == NULL) {
        seed = fopen(FILE_MOUNT_PATH "/README.txt", "wb");
        if (seed != NULL) {
            (void)fputs("ESP-Iris hardware file-service fixture\n", seed);
        }
    }
    if (seed != NULL) {
        (void)fclose(seed);
    } else {
        ESP_LOGW(TAG, "Unable to create the fixture README");
    }

    const esp_iris_file_volume_config_t volume = {
        .id = "fs",
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

static esp_err_t echo_rpc(const esp_iris_rpc_request_t *request,
                          uint8_t *response, size_t response_capacity,
                          size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size > response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response, request->payload, request->payload_size);
    *response_size = request->payload_size;
    return ESP_OK;
}

static esp_err_t screen_begin(const esp_iris_media_desc_t *requested,
                              esp_iris_media_desc_t *actual,
                              uint32_t *total_size, void *user_ctx)
{
    (void)requested;
    (void)user_ctx;
    *actual = (esp_iris_media_desc_t) {
        .width = 2,
        .height = 2,
        .stride = 4,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    };
    *total_size = 8;
    return ESP_OK;
}

static esp_err_t screen_read(uint32_t offset, uint8_t *out, size_t capacity,
                             size_t *out_size, void *user_ctx)
{
    static const uint8_t pixels[8] = {
        0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0xff, 0xff,
    };
    (void)user_ctx;
    if (offset >= sizeof(pixels) || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = sizeof(pixels) - offset;
    if (size > capacity) {
        size = capacity;
    }
    memcpy(out, pixels + offset, size);
    *out_size = size;
    return ESP_OK;
}

static void media_task(void *arg)
{
    (void)arg;
    const esp_iris_media_desc_t description = {
        .width = 1,
        .height = 1,
        .format = ESP_IRIS_PIXEL_FORMAT_JPEG,
        .quality = 80,
    };
    uint32_t frame_id = 0;
    while (true) {
        if (esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_IMAGE)) {
            const uint8_t sample[] = {0xff, 0xd8, 0xff, 0xd9};
            (void)esp_iris_media_submit(ESP_IRIS_CHANNEL_IMAGE, &description,
                                        ++frame_id, 0, sample,
                                        sizeof(sample));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    const esp_iris_screen_backend_t screen = {
        .begin = screen_begin,
        .read = screen_read,
    };
    register_file_volume();
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 1, echo_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_screen_register(&screen));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI(TAG,
             "IRIS_FILE_FIXTURE_READY volume=fs base=%s atomic_replace=0",
             FILE_MOUNT_PATH);
    xTaskCreate(media_task, "iris_media_test", 2048, NULL, 4, NULL);
}
