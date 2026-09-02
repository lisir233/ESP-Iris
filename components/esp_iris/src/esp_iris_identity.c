#include "esp_iris_internal.h"

#include <string.h>

#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define IRIS_NVS_NAMESPACE "esp_iris"
#define IRIS_NVS_DEVICE_ID "device_id"

esp_err_t iris_identity_load_or_create(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        nvs_flash_init_partition(CONFIG_ESP_IRIS_NVS_PARTITION_NAME);
    if (err != ESP_OK) {
        /* Iris must never erase shared NVS as an implicit recovery action. */
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open_from_partition(CONFIG_ESP_IRIS_NVS_PARTITION_NAME,
                                  IRIS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(runtime->device_id);
    err = nvs_get_blob(handle, IRIS_NVS_DEVICE_ID, runtime->device_id, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(runtime->device_id, sizeof(runtime->device_id));
        /* RFC 4122 variant/version bits make the stored value recognizable as
         * a random UUID while the wire representation stays 16 raw bytes. */
        runtime->device_id[6] = (runtime->device_id[6] & 0x0fU) | 0x40U;
        runtime->device_id[8] = (runtime->device_id[8] & 0x3fU) | 0x80U;
        err = nvs_set_blob(handle, IRIS_NVS_DEVICE_ID, runtime->device_id,
                           sizeof(runtime->device_id));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    } else if (err == ESP_OK && size != sizeof(runtime->device_id)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    do {
        esp_fill_random(&runtime->boot_id, sizeof(runtime->boot_id));
    } while (runtime->boot_id == 0);
    return ESP_OK;
}
