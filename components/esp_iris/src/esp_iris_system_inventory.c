#include "esp_iris_internal.h"

#include <string.h>

#include "esp_iris_system_inventory.h"

#if CONFIG_ESP_IRIS_SYSTEM_INVENTORY

typedef struct {
    bool registered;
    esp_iris_system_inventory_provider_t provider;
} iris_system_inventory_state_t;

static iris_system_inventory_state_t s_system_inventory;

esp_err_t esp_iris_system_inventory_register(
    const esp_iris_system_inventory_provider_t *provider)
{
    if (provider == NULL || provider->get_inventory == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_iris_is_started() || s_system_inventory.registered) {
        return ESP_ERR_INVALID_STATE;
    }
    s_system_inventory.provider = *provider;
    s_system_inventory.registered = true;
    return ESP_OK;
}

esp_err_t esp_iris_system_inventory_unregister(void *user_ctx)
{
    if (esp_iris_is_started() || !s_system_inventory.registered ||
        s_system_inventory.provider.user_ctx != user_ctx ||
        iris_system_update_backend_registered()) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_system_inventory, 0, sizeof(s_system_inventory));
    return ESP_OK;
}

bool iris_system_inventory_handle_frame(iris_runtime_t *runtime,
                                        const iris_decoded_frame_t *frame)
{
    if (frame->header.channel != ESP_IRIS_CHANNEL_SYSTEM_UPDATE ||
        frame->header.type != ESP_IRIS_SYSTEM_UPDATE_INVENTORY) {
        return false;
    }
    if (frame->header.payload_size != 0) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_SIZE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    if (!s_system_inventory.registered) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                               frame->header.type);
        return true;
    }

    esp_iris_system_inventory_t inventory = {0};
    const esp_err_t err = s_system_inventory.provider.get_inventory(
        &inventory, s_system_inventory.provider.user_ctx);
    if (err != ESP_OK) {
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    if ((inventory.flags & ~ESP_IRIS_SYSTEM_INVENTORY_VALID_FLAGS) != 0) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_ARG, frame->header.channel,
                               frame->header.type);
        return true;
    }

    uint8_t response[92];
    iris_put_le32(response, inventory.flags);
    iris_put_le32(response + 4, inventory.layout_version);
    memcpy(response + 8, inventory.bootloader_sha256, 32);
    memcpy(response + 40, inventory.partition_table_sha256, 32);
    memcpy(response + 72, inventory.last_operation_id, 16);
    iris_put_le32(response + 88, (uint32_t)inventory.last_result);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_INVENTORY_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE, frame->header.request_id,
                           0, response, sizeof(response));
    return true;
}

uint64_t iris_system_inventory_capabilities(void)
{
    return s_system_inventory.registered ? ESP_IRIS_CAP_SYSTEM_INVENTORY : 0;
}

uint32_t iris_system_inventory_static_bytes(void)
{
    return sizeof(s_system_inventory);
}

bool iris_system_inventory_registered(void)
{
    return s_system_inventory.registered;
}

#else

esp_err_t esp_iris_system_inventory_register(
    const esp_iris_system_inventory_provider_t *provider)
{
    (void)provider;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_system_inventory_unregister(void *user_ctx)
{
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

bool iris_system_inventory_handle_frame(iris_runtime_t *runtime,
                                        const iris_decoded_frame_t *frame)
{
    (void)runtime;
    (void)frame;
    return false;
}

uint64_t iris_system_inventory_capabilities(void)
{
    return 0;
}

uint32_t iris_system_inventory_static_bytes(void)
{
    return 0;
}

bool iris_system_inventory_registered(void)
{
    return false;
}

#endif
