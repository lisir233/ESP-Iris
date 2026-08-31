#include "esp_iris.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CRASH_SERVICE_ID 0x1400U
#define CRASH_STATE_METHOD_ID 1U
#define CRASH_RESUME_METHOD_ID 2U
#define CRASH_RETRY_METHOD_ID 3U
#define CRASH_NVS_NAMESPACE "iris_crash_demo"

typedef struct {
    uint32_t count;
    uint32_t limit;
    uint32_t app_address;
    bool injection_enabled;
    bool planned_restart;
} crash_state_t;

typedef enum {
    RECOVERY_COMMAND_RESUME = 1,
    RECOVERY_COMMAND_RETRY = 2,
} recovery_command_t;

static const char *TAG = "iris_crash_example";
static QueueHandle_t s_recovery_commands;

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
static void put_le32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}
#endif

static crash_state_t state_read(void)
{
    crash_state_t state = {
        .limit = CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_LIMIT,
#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_AUTO_CRASH
        .injection_enabled = true,
#endif
    };
    nvs_handle_t handle;
    if (nvs_open(CRASH_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return state;
    }
    (void)nvs_get_u32(handle, "count", &state.count);
    (void)nvs_get_u32(handle, "limit", &state.limit);
    (void)nvs_get_u32(handle, "app_addr", &state.app_address);
    uint8_t enabled = state.injection_enabled ? 1U : 0U;
    uint8_t planned = 0;
    (void)nvs_get_u8(handle, "inject", &enabled);
    (void)nvs_get_u8(handle, "planned", &planned);
    nvs_close(handle);
    state.injection_enabled = enabled != 0;
    state.planned_restart = planned != 0;
    if (state.limit < 1 || state.limit > 100) {
        state.limit = CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_LIMIT;
    }
    return state;
}

static esp_err_t state_write(const crash_state_t *state)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CRASH_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open crash state");
    esp_err_t err = nvs_set_u32(handle, "count", state->count);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "limit", state->limit);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "app_addr", state->app_address);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "inject",
                         state->injection_enabled ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "planned",
                         state->planned_restart ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
static const esp_partition_t *find_app(uint32_t address)
{
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL && partition->address == address &&
            partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
            partition->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            esp_partition_iterator_release(iterator);
            return partition;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return NULL;
}
#endif

esp_err_t esp_iris_platform_mark_planned_restart(void)
{
    crash_state_t state = state_read();
    state.planned_restart = true;
    return state_write(&state);
}

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY && CONFIG_ESP_IRIS_OTA
esp_err_t esp_iris_platform_prepare_ota(uint32_t running_address,
                                       uint32_t target_address)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL || running->address != running_address ||
        running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY ||
        find_app(target_address) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    crash_state_t state = state_read();
    state.count = 0;
    state.limit = CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_LIMIT;
    state.app_address = target_address;
    state.injection_enabled = true;
    state.planned_restart = false;
    return state_write(&state);
}
#endif

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
static esp_err_t state_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != 0 || response_capacity < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    const crash_state_t state = state_read();
    put_le32(response, state.count);
    put_le32(response + 4, state.limit);
    put_le32(response + 8, state.app_address);
    put_le32(response + 12,
             (state.injection_enabled ? 1U : 0U) |
             (state.planned_restart ? 2U : 0U));
    *response_size = 16;
    return ESP_OK;
}

static esp_err_t queue_recovery_command(const esp_iris_rpc_request_t *request,
                                        size_t *response_size,
                                        recovery_command_t command)
{
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const crash_state_t state = state_read();
    if (find_app(state.app_address) == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xQueueSend(s_recovery_commands, &command, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    *response_size = 0;
    return ESP_OK;
}

static esp_err_t resume_rpc(const esp_iris_rpc_request_t *request,
                            uint8_t *response, size_t response_capacity,
                            size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    return queue_recovery_command(request, response_size,
                                  RECOVERY_COMMAND_RESUME);
}

static esp_err_t retry_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    return queue_recovery_command(request, response_size,
                                  RECOVERY_COMMAND_RETRY);
}

static void recovery_task(void *arg)
{
    (void)arg;
    recovery_command_t command;
    while (xQueueReceive(s_recovery_commands, &command, portMAX_DELAY) ==
           pdTRUE) {
        crash_state_t state = state_read();
        const esp_partition_t *application = find_app(state.app_address);
        if (application == NULL) {
            ESP_LOGE(TAG, "stored application partition is unavailable");
            continue;
        }
        state.count = 0;
        state.injection_enabled = command == RECOVERY_COMMAND_RETRY;
        state.planned_restart = true;
        ESP_ERROR_CHECK(state_write(&state));
        ESP_ERROR_CHECK(esp_ota_set_boot_partition(application));
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
    vTaskDelete(NULL);
}
#endif

#if !CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
static void crash_or_stabilize_task(void *arg)
{
    (void)arg;
    crash_state_t state = state_read();
    if (!state.injection_enabled) {
        vTaskDelay(pdMS_TO_TICKS(
            CONFIG_ESP_IRIS_CRASH_EXAMPLE_STABLE_RESET_MS));
        state = state_read();
        state.count = 0;
        state.planned_restart = false;
        ESP_ERROR_CHECK(state_write(&state));
        ESP_LOGI(TAG, "stable runtime confirmed; crash count cleared");
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_AUTO_CRASH
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_DELAY_MS));
    state = state_read();
    ++state.count;
    state.limit = CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_LIMIT;
    state.planned_restart = false;
    if (state.count >= state.limit) {
        const esp_partition_t *factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        ESP_ERROR_CHECK(factory != NULL ? ESP_OK : ESP_ERR_NOT_FOUND);
        ESP_ERROR_CHECK(esp_ota_set_boot_partition(factory));
    }
    ESP_ERROR_CHECK(state_write(&state));
    ESP_LOGE(TAG, "injecting crash %" PRIu32 "/%" PRIu32,
             state.count, state.limit);
    fflush(stdout);
    abort();
#else
    vTaskDelete(NULL);
#endif
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
    s_recovery_commands = xQueueCreate(1, sizeof(recovery_command_t));
    ESP_ERROR_CHECK(s_recovery_commands != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_iris_rpc_register(CRASH_SERVICE_ID,
                                          CRASH_STATE_METHOD_ID,
                                          state_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(CRASH_SERVICE_ID,
                                          CRASH_RESUME_METHOD_ID,
                                          resume_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(CRASH_SERVICE_ID,
                                          CRASH_RETRY_METHOD_ID,
                                          retry_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGW(TAG, "RECOVERY: retained Core Dump is available through Iris");
    ESP_ERROR_CHECK(xTaskCreate(recovery_task, "crash_recovery", 3072, NULL,
                                4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_ERROR_CHECK(running != NULL ? ESP_OK : ESP_ERR_NOT_FOUND);
    crash_state_t state = state_read();
    state.limit = CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_LIMIT;
    state.app_address = running->address;
    state.planned_restart = false;
    ESP_ERROR_CHECK(state_write(&state));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI(TAG, "APPLICATION: crash=%u count=%" PRIu32 "/%" PRIu32,
             state.injection_enabled, state.count, state.limit);
    ESP_ERROR_CHECK(xTaskCreate(crash_or_stabilize_task, "crash_inject", 3072,
                                NULL, 4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}
