#include "esp_iris.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LIFECYCLE_SERVICE_ID 0x1300U
#define LIFECYCLE_CYCLE_METHOD_ID 1U

static const char *TAG = "iris_lifecycle";
static QueueHandle_t s_commands;

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
    static const uint8_t pixels[] = {
        0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0xff, 0xff,
    };
    (void)user_ctx;
    if (offset >= sizeof(pixels) || capacity == 0 || out == NULL ||
        out_size == NULL) {
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

static const esp_iris_screen_backend_t s_screen = {
    .begin = screen_begin,
    .read = screen_read,
};

static esp_err_t cycle_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const bool cycle = true;
    if (xQueueSend(s_commands, &cycle, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    *response_size = 0;
    return ESP_OK;
}

static void register_services(void)
{
    ESP_ERROR_CHECK(esp_iris_screen_register(&s_screen));
    ESP_ERROR_CHECK(esp_iris_rpc_register(LIFECYCLE_SERVICE_ID,
                                          LIFECYCLE_CYCLE_METHOD_ID,
                                          cycle_rpc, NULL));
}

static void lifecycle_task(void *arg)
{
    (void)arg;
    bool cycle;
    while (xQueueReceive(s_commands, &cycle, portMAX_DELAY) == pdTRUE) {
        (void)cycle;
        /* Give the worker time to transmit the RPC response before stopping. */
        vTaskDelay(pdMS_TO_TICKS(250));
        ESP_ERROR_CHECK(esp_iris_stop());
        ESP_ERROR_CHECK(esp_iris_is_started() ? ESP_FAIL : ESP_OK);
        ESP_LOGI(TAG, "STOPPED");

        ESP_ERROR_CHECK(esp_iris_rpc_unregister(LIFECYCLE_SERVICE_ID,
                                                LIFECYCLE_CYCLE_METHOD_ID));
        ESP_ERROR_CHECK(esp_iris_screen_unregister(NULL));
        ESP_LOGI(TAG, "UNREGISTERED");

        vTaskDelay(pdMS_TO_TICKS(
            CONFIG_ESP_IRIS_LIFECYCLE_EXAMPLE_RESTART_DELAY_MS));
        register_services();
        ESP_ERROR_CHECK(esp_iris_start());
        ESP_ERROR_CHECK(esp_iris_is_started() ? ESP_OK : ESP_FAIL);
        ESP_LOGI(TAG, "RESTARTED");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    s_commands = xQueueCreate(1, sizeof(bool));
    ESP_ERROR_CHECK(s_commands != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    register_services();
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(esp_iris_is_started() ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "RUNNING rpc=0x1300/1");
    ESP_ERROR_CHECK(xTaskCreate(lifecycle_task, "iris_lifecycle", 3072, NULL,
                                4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}
