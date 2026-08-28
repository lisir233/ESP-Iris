#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_IRIS_SYSTEM_OPERATION_ID_BYTES 16U
#define ESP_IRIS_SYSTEM_SHA256_BYTES 32U

enum {
    ESP_IRIS_SYSTEM_INVENTORY_BOOTLOADER_SHA256 = 1U << 0,
    ESP_IRIS_SYSTEM_INVENTORY_PARTITION_TABLE_SHA256 = 1U << 1,
    ESP_IRIS_SYSTEM_INVENTORY_LAST_OPERATION = 1U << 2,
    ESP_IRIS_SYSTEM_INVENTORY_VALID_FLAGS =
        ESP_IRIS_SYSTEM_INVENTORY_BOOTLOADER_SHA256 |
        ESP_IRIS_SYSTEM_INVENTORY_PARTITION_TABLE_SHA256 |
        ESP_IRIS_SYSTEM_INVENTORY_LAST_OPERATION,
};

typedef struct {
    uint32_t flags;
    uint32_t layout_version;
    uint8_t bootloader_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES];
    uint8_t partition_table_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES];
    uint8_t last_operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
    esp_err_t last_result;
} esp_iris_system_inventory_t;

/* Read-only provider for evidence calculated from the currently installed
 * Flash contents. A normal application may register this provider without
 * exposing any system-update writer. Recovery firmware normally registers
 * both the provider and the write backend.
 *
 * The product defines the protected ranges whose hashes are reported. The
 * same exact, erased-byte-padded ranges must be used by its bundle builder and
 * Flash backend so post-reboot comparisons are meaningful. get_inventory()
 * runs synchronously on the ESP-Iris worker and must be read-only. */
typedef struct {
    esp_err_t (*get_inventory)(esp_iris_system_inventory_t *inventory,
                               void *user_ctx);
    void *user_ctx;
} esp_iris_system_inventory_provider_t;

/* Register before esp_iris_start(). The provider is copied; user_ctx and all
 * referenced product state must remain valid until unregister. If a write
 * backend is also registered, unregister that backend first. */
esp_err_t esp_iris_system_inventory_register(
    const esp_iris_system_inventory_provider_t *provider);
esp_err_t esp_iris_system_inventory_unregister(void *user_ctx);

#ifdef __cplusplus
}
#endif
