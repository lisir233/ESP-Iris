#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_iris_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Local status value while multi-transport firmware has no candidate. */
    ESP_IRIS_TRANSPORT_KIND_NONE = 0,
    ESP_IRIS_TRANSPORT_KIND_USB = 1,
    ESP_IRIS_TRANSPORT_KIND_TCP = 2,
    ESP_IRIS_TRANSPORT_KIND_USB_SERIAL_JTAG = 3,
} esp_iris_transport_kind_t;

typedef enum {
    ESP_IRIS_LIFECYCLE_STOPPED = 0,
    ESP_IRIS_LIFECYCLE_STARTING = 1,
    ESP_IRIS_LIFECYCLE_RUNNING = 2,
    ESP_IRIS_LIFECYCLE_STOPPING = 3,
    ESP_IRIS_LIFECYCLE_FAILED = 4,
} esp_iris_lifecycle_t;

typedef struct {
    bool started;
    bool link_connected;
    bool session_ready;
    bool previous_boot_crash;
    bool core_dump_present;
    bool core_dump_valid;
    esp_iris_lifecycle_t lifecycle;
    esp_iris_transport_kind_t transport;
    uint8_t device_id[16];
    uint64_t boot_id;
    uint32_t session_id;
    uint64_t uptime_us;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t invalid_frames;
    uint32_t link_count;
    uint32_t log_dropped_bytes;
    uint32_t task_stack_free_min_bytes;
    uint32_t worker_active_max_us;
    uint32_t internal_heap_used_bytes;
    uint32_t static_internal_bytes;
    uint32_t core_dump_size;
    uint32_t reset_reason;
} esp_iris_status_t;

typedef struct {
    uint16_t service_id;
    uint16_t method_id;
    uint32_t request_id;
    uint32_t deadline_ms;
    const uint8_t *payload;
    size_t payload_size;
} esp_iris_rpc_request_t;

typedef esp_err_t (*esp_iris_rpc_handler_t)(
    const esp_iris_rpc_request_t *request, uint8_t *response,
    size_t response_capacity, size_t *response_size, void *user_ctx);

typedef enum {
    ESP_IRIS_JOB_QUEUED = 0,
    ESP_IRIS_JOB_RUNNING = 1,
    ESP_IRIS_JOB_SUCCEEDED = 2,
    ESP_IRIS_JOB_FAILED = 3,
    ESP_IRIS_JOB_CANCELLED = 4,
} esp_iris_job_state_t;

typedef struct esp_iris_job *esp_iris_job_handle_t;
typedef void (*esp_iris_job_cancel_fn)(void *user_ctx);

typedef struct {
    uint32_t id;
    uint16_t kind;
    esp_iris_job_state_t state;
    uint16_t progress_permille;
    esp_err_t result;
    bool cancel_requested;
} esp_iris_job_info_t;

typedef enum {
    ESP_IRIS_PIXEL_FORMAT_RGB565 = 1,
    ESP_IRIS_PIXEL_FORMAT_RGB888 = 2,
    ESP_IRIS_PIXEL_FORMAT_JPEG = 3,
    ESP_IRIS_PIXEL_FORMAT_PNG = 4,
    ESP_IRIS_AUDIO_FORMAT_PCM_S16LE = 0x100,
    ESP_IRIS_AUDIO_FORMAT_OPUS = 0x101,
} esp_iris_media_format_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t stride;
    uint16_t format;
    uint16_t quality;
} esp_iris_media_desc_t;

typedef struct {
    esp_err_t (*begin)(const esp_iris_media_desc_t *requested,
                       esp_iris_media_desc_t *actual, uint32_t *total_size,
                       void *user_ctx);
    esp_err_t (*read)(uint32_t offset, uint8_t *out, size_t capacity,
                      size_t *out_size, void *user_ctx);
    void (*end)(void *user_ctx);
    void *user_ctx;
} esp_iris_screen_backend_t;

#define ESP_IRIS_FILE_VOLUME_ID_MAX 15U
#define ESP_IRIS_FILE_PATH_MAX 255U

typedef struct {
    const char *id;
    const char *base_path;
    uint32_t capabilities;
} esp_iris_file_volume_config_t;

/* Idempotent. Configuration comes exclusively from Kconfig and NVS. */
esp_err_t esp_iris_start(void);
esp_err_t esp_iris_stop(void);
bool esp_iris_is_started(void);
esp_err_t esp_iris_get_status(esp_iris_status_t *out_status);

/* Optional product lifecycle marker. State is replayed to a newly connected
 * PC session, so callers do not need to wait for a link. */
esp_err_t esp_iris_mark_planned_restart(void);

/* Product recovery glue may provide strong platform hook implementations.
 * The base component returns ESP_ERR_NOT_SUPPORTED: Iris start is never
 * mistaken for product acceptance, and OTA cannot select a boot slot until
 * prepare_ota has persisted the product's recovery metadata. */
esp_err_t esp_iris_mark_healthy(void);
esp_err_t esp_iris_platform_mark_healthy(void);
esp_err_t esp_iris_platform_mark_planned_restart(void);
esp_err_t esp_iris_platform_select_ota_target(uint32_t default_address,
                                               uint32_t *target_address);
esp_err_t esp_iris_platform_prepare_ota(uint32_t running_address,
                                        uint32_t target_address);

/* Writes a lowercase, NUL-terminated 32-character device ID. */
esp_err_t esp_iris_format_device_id(char out[33]);

/* M6: bounded binary RPC and long-running jobs. Registration allocates only
 * the small handler table and is intended to run during product startup. */
esp_err_t esp_iris_rpc_register(uint16_t service_id, uint16_t method_id,
                                esp_iris_rpc_handler_t handler,
                                void *user_ctx);
esp_err_t esp_iris_rpc_unregister(uint16_t service_id, uint16_t method_id);
esp_err_t esp_iris_job_create(uint16_t kind, esp_iris_job_cancel_fn cancel,
                              void *user_ctx,
                              esp_iris_job_handle_t *out_job);
esp_err_t esp_iris_job_update(esp_iris_job_handle_t job,
                              uint16_t progress_permille);
esp_err_t esp_iris_job_finish(esp_iris_job_handle_t job, esp_err_t result);
bool esp_iris_job_cancel_requested(esp_iris_job_handle_t job);
esp_err_t esp_iris_job_get_info(esp_iris_job_handle_t job,
                                esp_iris_job_info_t *out_info);

/* M7-M9: optional zero-copy screenshot provider and bounded latest-chunk
 * media submission. Nothing is allocated until a backend/stream is used. */
esp_err_t esp_iris_screen_register(const esp_iris_screen_backend_t *backend);
esp_err_t esp_iris_screen_unregister(void *user_ctx);
esp_err_t esp_iris_media_submit(esp_iris_channel_t channel,
                                const esp_iris_media_desc_t *description,
                                uint32_t frame_id, uint16_t flags,
                                const void *data, size_t size);
bool esp_iris_media_is_streaming(esp_iris_channel_t channel);

/* Register product-owned logical volumes before esp_iris_start(). Paths sent
 * on the wire are UTF-8 relative paths and are resolved only below base_path.
 * Capabilities explicitly opt each volume into read and mutation operations;
 * write data is hashed and staged in a same-directory temporary file. */
esp_err_t esp_iris_file_volume_register(
    const esp_iris_file_volume_config_t *config);
esp_err_t esp_iris_file_volume_unregister(const char *id);

/* M10: only supported when TCP pairing is enabled. The token is 64 lowercase
 * hex characters plus NUL and is stored as 32 random bytes in NVS. */
esp_err_t esp_iris_pairing_token_get(char out[65]);
esp_err_t esp_iris_pairing_token_rotate(char out[65]);
esp_err_t esp_iris_pairing_token_set(const char token[65]);

#ifdef __cplusplus
}
#endif
