#include "esp_iris.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
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
    uint32_t app_address;
    bool injection_enabled;
} example_state_t;

typedef enum {
    RECOVERY_COMMAND_RESUME = 1,
    RECOVERY_COMMAND_RETRY = 2,
} recovery_command_t;

static const char *TAG = "iris_crash_example";

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
static QueueHandle_t s_recovery_commands;

static void put_le32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}
#endif

static example_state_t example_state_read(void)
{
    example_state_t state = {
#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_AUTO_CRASH
        .injection_enabled = true,
#endif
    };
    nvs_handle_t handle;
    if (nvs_open(CRASH_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return state;
    }
    (void)nvs_get_u32(handle, "app_addr", &state.app_address);
    uint8_t enabled = state.injection_enabled ? 1U : 0U;
    (void)nvs_get_u8(handle, "inject", &enabled);
    nvs_close(handle);
    state.injection_enabled = enabled != 0;
    return state;
}

static esp_err_t example_state_write(const example_state_t *state)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CRASH_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open example state");
    esp_err_t err = nvs_set_u32(handle, "app_addr", state->app_address);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "inject",
                         state->injection_enabled ? 1U : 0U);
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
    return NULL;
}
#endif

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
    example_state_t state = {
        .app_address = target_address,
        .injection_enabled = true,
    };
    return example_state_write(&state);
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
    esp_iris_status_t status;
    ESP_RETURN_ON_ERROR(esp_iris_get_status(&status), TAG, "read Iris status");
    const example_state_t state = example_state_read();
    put_le32(response, status.crash_count);
    put_le32(response + 4, status.crash_limit);
    put_le32(response + 8, state.app_address);
    put_le32(response + 12,
             (state.injection_enabled ? 1U : 0U) |
             (status.crash_recovery_pending ? 2U : 0U));
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
    const example_state_t state = example_state_read();
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
        example_state_t state = example_state_read();
        const esp_partition_t *application = find_app(state.app_address);
        if (application == NULL) {
            ESP_LOGE(TAG, "stored application partition is unavailable");
            continue;
        }
        state.injection_enabled = command == RECOVERY_COMMAND_RETRY;
        ESP_ERROR_CHECK(example_state_write(&state));
        ESP_ERROR_CHECK(esp_iris_crash_loop_reset());
        ESP_ERROR_CHECK(esp_iris_mark_planned_restart());
        ESP_ERROR_CHECK(esp_ota_set_boot_partition(application));
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
    vTaskDelete(NULL);
}
#endif

#if !CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY
esp_err_t esp_iris_platform_mark_healthy(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

static void crash_or_stabilize_task(void *arg)
{
    (void)arg;
    const example_state_t state = example_state_read();
    if (!state.injection_enabled) {
        ESP_ERROR_CHECK(esp_iris_mark_healthy());
        esp_rom_printf("IRIS_CRASH_STABLE_HEALTHY\n");
        ESP_LOGI(TAG, "stable runtime confirmed; Iris crash count cleared");
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_ESP_IRIS_CRASH_EXAMPLE_AUTO_CRASH
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_DELAY_MS));
    esp_iris_status_t status;
    ESP_ERROR_CHECK(esp_iris_get_status(&status));
    ESP_LOGE(TAG, "injecting unplanned crash; retained count=%" PRIu32
                  "/%" PRIu32, status.crash_count, status.crash_limit);
    fflush(stdout);
    abort();
#else
    ESP_ERROR_CHECK(esp_iris_mark_healthy());
    esp_rom_printf("IRIS_CRASH_STABLE_HEALTHY\n");
    vTaskDelete(NULL);
#endif
}
#endif

void app_main(void)
{
    /* Keep this before product initialization: the next boot can then
     * attribute crashes from every later app_main step to this image. */
    const esp_err_t probe_err = esp_iris_boot_probe();
    ESP_ERROR_CHECK(nvs_flash_init());
    if (probe_err != ESP_OK) {
        ESP_LOGE(TAG, "early Iris crash probe failed: %s",
                 esp_err_to_name(probe_err));
    }

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
    esp_iris_status_t status;
    ESP_ERROR_CHECK(esp_iris_get_status(&status));
    esp_rom_printf("IRIS_CRASH_RECOVERY_READY mode=recovery count=%" PRIu32
                   " limit=%" PRIu32 "\n", status.crash_count,
                   status.crash_limit);
    ESP_LOGW(TAG, "RECOVERY: retained Core Dump is available through Iris");
    ESP_ERROR_CHECK(xTaskCreate(recovery_task, "crash_recovery", 3072, NULL,
                                4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_ERROR_CHECK(running != NULL ? ESP_OK : ESP_ERR_NOT_FOUND);
    example_state_t state = example_state_read();
    state.app_address = running->address;
    ESP_ERROR_CHECK(example_state_write(&state));
    ESP_ERROR_CHECK(esp_iris_start());
    esp_iris_status_t status;
    ESP_ERROR_CHECK(esp_iris_get_status(&status));
    esp_rom_printf("IRIS_CRASH_RECOVERY_READY mode=application count=%" PRIu32
                   " limit=%" PRIu32 " auto_crash=%u\n",
                   status.crash_count, status.crash_limit,
                   state.injection_enabled ? 1U : 0U);
    ESP_LOGI(TAG, "APPLICATION: crash=%u count=%" PRIu32 "/%" PRIu32,
             state.injection_enabled, status.crash_count, status.crash_limit);
    ESP_ERROR_CHECK(xTaskCreate(crash_or_stabilize_task, "crash_inject", 3072,
                                NULL, 4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}
