#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_iris.h"
#include "esp_iris_codec.h"
#include "esp_iris_state.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define IRIS_LOG_RECORD_DATA_MAX 240U
#define IRIS_LOG_RECORD_HEADER_SIZE 12U

typedef enum {
    IRIS_LINK_EVENT_NONE,
    IRIS_LINK_EVENT_CONNECTED,
    IRIS_LINK_EVENT_DISCONNECTED,
} iris_link_event_t;

typedef struct {
    int listen_fd;
    int client_fd;
    bool link_up;
    bool reported_link_up;
    bool driver_started;
    atomic_bool host_open;
    atomic_bool disconnect_pending;
    char usb_serial[33];
    int usb_serial_jtag_install_core;
    bool usb_serial_jtag_auto_reset_was_disabled;
} iris_transport_state_t;

struct iris_runtime;

typedef struct {
    esp_iris_transport_kind_t kind;
    const char *name;
    esp_err_t (*start)(struct iris_runtime *runtime,
                       iris_transport_state_t *state);
    void (*stop)(struct iris_runtime *runtime, iris_transport_state_t *state);
    iris_link_event_t (*poll)(struct iris_runtime *runtime,
                              iris_transport_state_t *state);
    void (*disconnect)(struct iris_runtime *runtime,
                       iris_transport_state_t *state);
    int (*read)(struct iris_runtime *runtime, iris_transport_state_t *state,
                uint8_t *buffer, size_t capacity);
    int (*write)(struct iris_runtime *runtime, iris_transport_state_t *state,
                 const uint8_t *buffer, size_t length);
} iris_transport_ops_t;

typedef struct {
    iris_transport_state_t usb;
    iris_transport_state_t tcp;
    iris_transport_state_t usb_serial_jtag;
    const iris_transport_ops_t *active_ops;
    iris_transport_state_t *active_state;
    int64_t claim_deadline_us;
    int64_t retry_after_us[4];
    uint8_t scan_start;
    bool committed;
} iris_transport_manager_t;

typedef struct {
    uint64_t monotonic_us;
    uint32_t dropped_total;
    uint8_t source;
    uint8_t flags;
    uint16_t length;
    uint8_t data[IRIS_LOG_RECORD_DATA_MAX];
} iris_log_record_t;

typedef struct iris_runtime {
    bool started;
    bool initializing;
    bool running;
    bool identity_ready;
    bool crash_initialized;
    bool vfs_registered;
    bool stdio_redirected;
    bool link_connected;
    bool hello_acked;
    bool healthy;
    bool previous_boot_crash;
    bool core_dump_present;
    bool core_dump_valid;
    bool core_dump_checked;
    esp_iris_lifecycle_t lifecycle;
    iris_session_state_t session_state;

    uint8_t device_id[16];
    uint64_t boot_id;
    uint32_t session_id;
    uint32_t sequence[ESP_IRIS_CHANNEL_COUNT];
    uint32_t log_credit;
    int64_t next_hello_us;
    uint32_t pending_events;

    TaskHandle_t task;
    iris_transport_manager_t transport;

    uint8_t rx_wire[ESP_IRIS_MAX_WIRE_FRAME_SIZE];
    size_t rx_wire_length;
    bool rx_discarding;
    bool disconnect_after_tx;

    uint8_t tx_wire[ESP_IRIS_MAX_WIRE_FRAME_SIZE];
    size_t tx_wire_length;
    size_t tx_wire_offset;

    portMUX_TYPE log_lock;
    portMUX_TYPE event_lock;
    size_t log_tail;
    size_t log_used;
    uint32_t log_dropped_bytes;

    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t invalid_frames;
    uint32_t link_count;
    uint32_t task_stack_free_min_bytes;
    uint32_t worker_active_max_us;
    uint32_t internal_heap_used_bytes;
    uint32_t service_bytes_at_start;

    const esp_partition_t *core_dump_partition;
    size_t core_dump_address;
    size_t core_dump_size;
    char core_dump_elf_sha256[65];
    uint8_t core_dump_elf_sha256_length;
    char panic_reason[128];
} iris_runtime_t;

extern iris_runtime_t g_iris;

esp_err_t iris_identity_load_or_create(iris_runtime_t *runtime);

esp_err_t iris_log_vfs_init(iris_runtime_t *runtime);
esp_err_t iris_log_vfs_deinit(void);
esp_err_t iris_log_redirect_stdio(void);
esp_err_t iris_log_restore_stdio(void);
bool iris_log_pop(iris_runtime_t *runtime, size_t payload_budget,
                  iris_log_record_t *out_record);

void iris_crash_probe(iris_runtime_t *runtime);
esp_err_t iris_crash_build_metadata(iris_runtime_t *runtime, uint8_t *out,
                                    size_t capacity, size_t *out_size);
esp_err_t iris_crash_read(iris_runtime_t *runtime, size_t offset,
                          size_t maximum, uint8_t *out, size_t *out_size);

esp_err_t iris_files_init(iris_runtime_t *runtime);
void iris_files_deinit(void);
void iris_files_session_end(uint32_t session_id);
bool iris_files_handle_frame(iris_runtime_t *runtime,
                             const iris_decoded_frame_t *frame);
bool iris_files_queue_next(iris_runtime_t *runtime);
uint64_t iris_files_capabilities(void);
uint32_t iris_files_allocated_bytes(void);
uint32_t iris_files_static_bytes(void);

void iris_system_update_session_end(void);
bool iris_system_inventory_handle_frame(iris_runtime_t *runtime,
                                        const iris_decoded_frame_t *frame);
uint64_t iris_system_inventory_capabilities(void);
uint32_t iris_system_inventory_static_bytes(void);
bool iris_system_inventory_registered(void);
bool iris_system_update_handle_frame(iris_runtime_t *runtime,
                                     const iris_decoded_frame_t *frame);
uint64_t iris_system_update_capabilities(void);
uint32_t iris_system_update_static_bytes(void);
bool iris_system_update_backend_registered(void);

esp_err_t iris_queue_frame(iris_runtime_t *runtime, uint8_t channel,
                           uint8_t type, uint16_t flags,
                           uint32_t request_id, uint32_t stream_id,
                           const uint8_t *payload, size_t payload_size);
esp_err_t iris_queue_error(iris_runtime_t *runtime, uint32_t request_id,
                           esp_err_t code, uint8_t channel, uint8_t type);

esp_err_t iris_services_init(iris_runtime_t *runtime);
void iris_services_deinit(iris_runtime_t *runtime);
void iris_services_session_begin(iris_runtime_t *runtime);
void iris_services_session_end(iris_runtime_t *runtime);
bool iris_services_handle_frame(iris_runtime_t *runtime,
                                const iris_decoded_frame_t *frame,
                                uint64_t received_us);
bool iris_services_queue_next(iris_runtime_t *runtime);
void iris_services_poll(iris_runtime_t *runtime);
uint64_t iris_services_capabilities(void);
uint8_t iris_services_auth_mode(void);
const uint8_t *iris_services_auth_challenge(size_t *size);
esp_err_t iris_services_authenticate(const iris_runtime_t *runtime,
                                     const uint8_t *payload, size_t size);
bool iris_services_credit(uint8_t channel, uint32_t amount);
uint32_t iris_services_allocated_bytes(void);
uint32_t iris_services_static_bytes(void);

esp_err_t iris_transport_start(iris_runtime_t *runtime);
void iris_transport_stop(iris_runtime_t *runtime);
iris_link_event_t iris_transport_poll(iris_runtime_t *runtime);
int iris_transport_read(iris_runtime_t *runtime, uint8_t *buffer,
                        size_t capacity);
int iris_transport_write(iris_runtime_t *runtime, const uint8_t *buffer,
                         size_t length);
esp_iris_transport_kind_t iris_transport_kind(void);
const char *iris_transport_name(void);
void iris_transport_commit(iris_runtime_t *runtime);
void iris_transport_disconnect(iris_runtime_t *runtime);

#if CONFIG_ESP_IRIS_TRANSPORT_USB
extern const iris_transport_ops_t g_iris_usb_transport_ops;
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_TCP
extern const iris_transport_ops_t g_iris_tcp_transport_ops;
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG
extern const iris_transport_ops_t g_iris_usb_serial_jtag_transport_ops;
#endif
