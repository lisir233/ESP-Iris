#include "esp_iris.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_err_t disabled_rpc(const esp_iris_rpc_request_t *request,
                              uint8_t *response, size_t response_capacity,
                              size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)response;
    (void)response_capacity;
    (void)response_size;
    (void)user_ctx;
    return ESP_OK;
}

static esp_err_t disabled_screen_begin(
    const esp_iris_media_desc_t *requested, esp_iris_media_desc_t *actual,
    uint32_t *total_size, void *user_ctx)
{
    (void)requested;
    (void)actual;
    (void)total_size;
    (void)user_ctx;
    return ESP_OK;
}

static esp_err_t disabled_screen_read(uint32_t offset, uint8_t *out,
                                      size_t capacity, size_t *out_size,
                                      void *user_ctx)
{
    (void)offset;
    (void)out;
    (void)capacity;
    (void)out_size;
    (void)user_ctx;
    return ESP_OK;
}

void app_main(void)
{
    const esp_iris_screen_backend_t screen = {
        .begin = disabled_screen_begin,
        .read = disabled_screen_read,
    };
    const esp_iris_file_volume_config_t volume = {
        .id = "disabled",
        .base_path = "/disabled",
        .capabilities = ESP_IRIS_FILE_VOLUME_READ,
    };
    const esp_iris_media_desc_t media = {
        .width = 1,
        .height = 1,
        .stride = 2,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    };
    const uint8_t pixel[2] = {0, 0};
    esp_iris_status_t status;
    esp_iris_job_handle_t job = NULL;
    esp_iris_job_info_t job_info;
    char token[65];
    char device_id[33];
    char valid_token[65];
    memset(valid_token, '0', sizeof(valid_token) - 1U);
    valid_token[sizeof(valid_token) - 1U] = '\0';

    const esp_err_t results[] = {
        esp_iris_start(),
        esp_iris_stop(),
        esp_iris_get_status(&status),
        esp_iris_mark_healthy(),
        esp_iris_mark_planned_restart(),
        esp_iris_rpc_register(1, 1, disabled_rpc, NULL),
        esp_iris_rpc_unregister(1, 1),
        esp_iris_job_create(1, NULL, NULL, &job),
        esp_iris_job_update(job, 1),
        esp_iris_job_finish(job, ESP_OK),
        esp_iris_job_get_info(job, &job_info),
        esp_iris_screen_register(&screen),
        esp_iris_screen_unregister(NULL),
        esp_iris_media_submit(ESP_IRIS_CHANNEL_IMAGE, &media, 1, 0, pixel,
                              sizeof(pixel)),
        esp_iris_file_volume_register(&volume),
        esp_iris_file_volume_unregister("disabled"),
        esp_iris_pairing_token_get(token),
        esp_iris_pairing_token_rotate(token),
        esp_iris_pairing_token_set(valid_token),
        esp_iris_format_device_id(device_id),
    };
    bool safe = !esp_iris_is_started() &&
                !esp_iris_job_cancel_requested(job) &&
                !esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_IMAGE);
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        safe = safe && results[i] == ESP_ERR_NOT_SUPPORTED;
    }
    safe = safe && !status.started && status.lifecycle == 0 &&
           token[0] == '\0' && device_id[0] == '\0';
    for (size_t attempt = 0; attempt < 20; ++attempt) {
        printf("IRIS_DISABLED_STATE schema=1 safe=%u calls=%u started=%u\n",
               safe ? 1U : 0U,
               (unsigned)(sizeof(results) / sizeof(results[0])),
               esp_iris_is_started() ? 1U : 0U);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
