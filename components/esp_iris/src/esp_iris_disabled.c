#include "esp_iris.h"
#include "esp_iris_system_inventory.h"
#include "esp_iris_system_update.h"

#include <string.h>

esp_err_t esp_iris_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_iris_is_started(void)
{
    return false;
}

esp_err_t esp_iris_get_status(esp_iris_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_mark_healthy(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_mark_planned_restart(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_rpc_register(uint16_t service_id, uint16_t method_id,
                                esp_iris_rpc_handler_t handler,
                                void *user_ctx)
{
    (void)service_id;
    (void)method_id;
    (void)handler;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_rpc_unregister(uint16_t service_id, uint16_t method_id)
{
    (void)service_id;
    (void)method_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_job_create(uint16_t kind, esp_iris_job_cancel_fn cancel,
                              void *user_ctx,
                              esp_iris_job_handle_t *out_job)
{
    (void)kind;
    (void)cancel;
    (void)user_ctx;
    (void)out_job;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_job_update(esp_iris_job_handle_t job,
                              uint16_t progress_permille)
{
    (void)job;
    (void)progress_permille;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_job_finish(esp_iris_job_handle_t job, esp_err_t result)
{
    (void)job;
    (void)result;
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_iris_job_cancel_requested(esp_iris_job_handle_t job)
{
    (void)job;
    return false;
}

esp_err_t esp_iris_job_get_info(esp_iris_job_handle_t job,
                                esp_iris_job_info_t *out_info)
{
    (void)job;
    (void)out_info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_screen_register(const esp_iris_screen_backend_t *backend)
{
    (void)backend;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_screen_unregister(void *user_ctx)
{
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_media_submit(esp_iris_channel_t channel,
                                const esp_iris_media_desc_t *description,
                                uint32_t frame_id, uint16_t flags,
                                const void *data, size_t size)
{
    (void)channel;
    (void)description;
    (void)frame_id;
    (void)flags;
    (void)data;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_iris_media_is_streaming(esp_iris_channel_t channel)
{
    (void)channel;
    return false;
}

esp_err_t esp_iris_file_volume_register(
    const esp_iris_file_volume_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_file_volume_unregister(const char *id)
{
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_pairing_token_get(char out[65])
{
    if (out != NULL) {
        out[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_pairing_token_rotate(char out[65])
{
    if (out != NULL) {
        out[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_pairing_token_set(const char token[65])
{
    (void)token;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_format_device_id(char out[33])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_system_update_register(
    const esp_iris_system_update_backend_t *backend)
{
    (void)backend;
    return ESP_ERR_NOT_SUPPORTED;
}

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

esp_err_t esp_iris_system_update_unregister(void *user_ctx)
{
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_system_update_get_status(
    esp_iris_system_update_status_t *out_status)
{
    (void)out_status;
    return ESP_ERR_NOT_SUPPORTED;
}
