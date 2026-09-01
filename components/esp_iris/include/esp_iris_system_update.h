#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_iris_system_inventory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER = 1,
    ESP_IRIS_SYSTEM_UPDATE_COMPONENT_PARTITION_TABLE = 2,
    ESP_IRIS_SYSTEM_UPDATE_COMPONENT_APPLICATION = 3,
    ESP_IRIS_SYSTEM_UPDATE_COMPONENT_RECOVERY = 4,
    ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA = 5,
} esp_iris_system_update_component_kind_t;

typedef enum {
    ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE = 0,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED = 1,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING = 2,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED = 3,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING = 4,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED = 5,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED = 6,
    ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED = 7,
} esp_iris_system_update_phase_t;

typedef struct {
    uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
    const uint8_t *manifest;
    size_t manifest_size;
    const uint8_t *signature;
    size_t signature_size;
    uint8_t manifest_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES];
    uint8_t component_count;
    uint8_t flags;
} esp_iris_system_update_manifest_t;

typedef struct {
    uint8_t id;
    esp_iris_system_update_component_kind_t kind;
    uint16_t flags;
    uint32_t target_offset;
    uint32_t size;
    uint8_t sha256[ESP_IRIS_SYSTEM_SHA256_BYTES];
} esp_iris_system_update_component_t;

typedef struct {
    uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
    esp_iris_system_update_phase_t phase;
    uint8_t component_count;
    uint8_t completed_components;
    uint8_t active_component_id;
    uint32_t component_received;
    uint32_t component_size;
    esp_err_t result;
} esp_iris_system_update_status_t;

/* The backend is the product authorization and Flash-policy boundary.
 * ESP-Iris never interprets target offsets as permission to write Flash.
 * prepare() must parse and authorize the manifest, optionally authenticating
 * it when the product requires signed updates. Every component callback must
 * verify that the descriptor agrees with the authorized plan.
 *
 * Sensitive images such as the bootloader and partition table should be held
 * in internal RAM by the backend and written only from commit(). commit()
 * must leave the link alive long enough for ESP-Iris to queue its response;
 * the product may schedule a delayed restart afterwards. ESP-Iris does not
 * add redundant bootloader/partition-table slots: products accepting
 * power-loss bricking may implement a single-copy, bootloader-first,
 * partition-table-last commit with mandatory write readback. */
typedef struct {
    esp_err_t (*prepare)(const esp_iris_system_update_manifest_t *manifest,
                         void *user_ctx);
    esp_err_t (*begin_component)(
        const esp_iris_system_update_component_t *component, void *user_ctx);
    esp_err_t (*write_component)(
        const esp_iris_system_update_component_t *component, uint32_t offset,
        const uint8_t *data, size_t size, void *user_ctx);
    esp_err_t (*end_component)(
        const esp_iris_system_update_component_t *component,
        const uint8_t actual_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES],
        void *user_ctx);
    esp_err_t (*commit)(
        const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
        void *user_ctx);
    void (*abort)(
        const uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES],
        esp_err_t reason, void *user_ctx);
    void *user_ctx;
} esp_iris_system_update_backend_t;

/* Register before esp_iris_start() and after the inventory provider. The
 * backend is copied; user_ctx and all referenced product state must remain
 * valid until unregister. Manifest/component pointers passed to callbacks are
 * valid only for the duration of that callback and must be copied if needed. */
esp_err_t esp_iris_system_update_register(
    const esp_iris_system_update_backend_t *backend);
esp_err_t esp_iris_system_update_unregister(void *user_ctx);
esp_err_t esp_iris_system_update_get_status(
    esp_iris_system_update_status_t *out_status);

#ifdef __cplusplus
}
#endif
