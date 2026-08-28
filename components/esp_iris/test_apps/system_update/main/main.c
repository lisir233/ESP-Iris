#include "esp_iris.h"
#include "esp_iris_system_update.h"

#include <string.h>

#include "esp_log.h"

typedef struct {
    esp_iris_system_inventory_t inventory;
    esp_iris_system_update_component_t active;
    uint32_t received;
} fixture_state_t;

static const char *TAG = "iris_system_fixture";
static fixture_state_t s_fixture;

static esp_err_t prepare(
    const esp_iris_system_update_manifest_t *manifest, void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    if (manifest == NULL || state == NULL || manifest->manifest_size == 0 ||
        manifest->signature_size == 0 || manifest->component_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&state->active, 0, sizeof(state->active));
    state->received = 0;
    return ESP_OK;
}

static esp_err_t begin_component(
    const esp_iris_system_update_component_t *component, void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    if (component == NULL || state == NULL || component->size == 0 ||
        state->active.id != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    state->active = *component;
    state->received = 0;
    return ESP_OK;
}

static esp_err_t write_component(
    const esp_iris_system_update_component_t *component, uint32_t offset,
    const uint8_t *data, size_t size, void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    if (component == NULL || data == NULL || state == NULL ||
        component->id != state->active.id || offset != state->received ||
        size == 0 || size > component->size - state->received) {
        return ESP_ERR_INVALID_SIZE;
    }
    state->received += (uint32_t)size;
    return ESP_OK;
}

static esp_err_t end_component(
    const esp_iris_system_update_component_t *component,
    const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES],
    void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    if (component == NULL || actual_sha256 == NULL || state == NULL ||
        component->id != state->active.id ||
        state->received != component->size) {
        return ESP_ERR_INVALID_STATE;
    }
    if (component->kind == ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER) {
        memcpy(state->inventory.bootloader_sha256, actual_sha256, 32);
        state->inventory.flags |=
            ESP_IRIS_SYSTEM_INVENTORY_BOOTLOADER_SHA256;
    } else if (component->kind ==
               ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE) {
        memcpy(state->inventory.partition_table_sha256, actual_sha256, 32);
        state->inventory.flags |=
            ESP_IRIS_SYSTEM_INVENTORY_PARTITION_TABLE_SHA256;
    }
    memset(&state->active, 0, sizeof(state->active));
    state->received = 0;
    return ESP_OK;
}

static esp_err_t commit(
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
    void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    if (operation_id == NULL || state == NULL || state->active.id != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(state->inventory.last_operation_id, operation_id, 16);
    state->inventory.flags |=
        ESP_IRIS_SYSTEM_INVENTORY_LAST_OPERATION;
    state->inventory.last_result = ESP_OK;
    return ESP_OK;
}

static esp_err_t get_inventory(
    esp_iris_system_inventory_t *inventory, void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    if (inventory == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *inventory = state->inventory;
    return ESP_OK;
}

static void abort_update(
    const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
    esp_err_t reason, void *user_ctx)
{
    fixture_state_t *state = user_ctx;
    (void)operation_id;
    if (state != NULL) {
        memset(&state->active, 0, sizeof(state->active));
        state->received = 0;
        state->inventory.last_result = reason;
    }
}

void app_main(void)
{
    const esp_iris_system_update_backend_t backend = {
        .prepare = prepare,
        .begin_component = begin_component,
        .write_component = write_component,
        .end_component = end_component,
        .commit = commit,
        .abort = abort_update,
        .user_ctx = &s_fixture,
    };
    const esp_iris_system_inventory_provider_t inventory_provider = {
        .get_inventory = get_inventory,
        .user_ctx = &s_fixture,
    };
    ESP_ERROR_CHECK(esp_iris_system_inventory_register(&inventory_provider));
    ESP_ERROR_CHECK(esp_iris_system_update_register(&backend));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI(TAG, "IRIS_SYSTEM_UPDATE_FIXTURE_READY non_flashing=1");
}
