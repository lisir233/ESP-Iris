#include "esp_iris_internal.h"

#include <limits.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#define IRIS_HELLO_INTERVAL_US (1000LL * 1000LL)
#define IRIS_LOG_PAYLOAD_HEADER_SIZE 16U
#define IRIS_EVENT_BIT(type) (1UL << (uint32_t)(type))
#define IRIS_STDIO_STATIC_BYTES (512U + 4U * sizeof(void *))
#define IRIS_STOP_TIMEOUT_MS 1000U

iris_runtime_t g_iris = {
    .transport = {
        .listen_fd = -1,
        .client_fd = -1,
    },
    .log_lock = portMUX_INITIALIZER_UNLOCKED,
    .event_lock = portMUX_INITIALIZER_UNLOCKED,
    .lifecycle = ESP_IRIS_LIFECYCLE_STOPPED,
    .task_stack_free_min_bytes = UINT32_MAX,
};

static portMUX_TYPE s_start_lock = portMUX_INITIALIZER_UNLOCKED;

static bool transition_lifecycle(iris_runtime_t *runtime,
                                 esp_iris_lifecycle_t requested)
{
    return iris_lifecycle_transition(runtime->lifecycle, requested,
                                     &runtime->lifecycle);
}

static bool transition_session(iris_runtime_t *runtime,
                               iris_session_event_t event)
{
    iris_session_state_t next = runtime->session_state;
    if (!iris_session_transition(runtime->session_state, event, &next)) {
        return false;
    }
    runtime->session_state = next;
    runtime->link_connected = next != IRIS_SESSION_DISCONNECTED;
    runtime->hello_acked = next == IRIS_SESSION_READY;
    return true;
}

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
} tlv_writer_t;

static bool tlv_put(tlv_writer_t *writer, uint8_t tag, const void *value,
                    size_t length)
{
    if (length > UINT16_MAX || writer->length + 3U + length > writer->capacity) {
        return false;
    }
    writer->data[writer->length++] = tag;
    iris_put_le16(writer->data + writer->length, (uint16_t)length);
    writer->length += 2;
    if (length > 0) {
        memcpy(writer->data + writer->length, value, length);
        writer->length += length;
    }
    return true;
}

static bool tlv_put_u8(tlv_writer_t *writer, uint8_t tag, uint8_t value)
{
    return tlv_put(writer, tag, &value, sizeof(value));
}

static bool tlv_put_u16(tlv_writer_t *writer, uint8_t tag, uint16_t value)
{
    uint8_t bytes[2];
    iris_put_le16(bytes, value);
    return tlv_put(writer, tag, bytes, sizeof(bytes));
}

static bool tlv_put_u32(tlv_writer_t *writer, uint8_t tag, uint32_t value)
{
    uint8_t bytes[4];
    iris_put_le32(bytes, value);
    return tlv_put(writer, tag, bytes, sizeof(bytes));
}

static bool tlv_put_u64(tlv_writer_t *writer, uint8_t tag, uint64_t value)
{
    uint8_t bytes[8];
    iris_put_le64(bytes, value);
    return tlv_put(writer, tag, bytes, sizeof(bytes));
}

static bool tlv_put_string(tlv_writer_t *writer, uint8_t tag,
                           const char *value, size_t maximum)
{
    return tlv_put(writer, tag, value, strnlen(value, maximum));
}

static esp_err_t queue_frame(iris_runtime_t *runtime, uint8_t channel,
                             uint8_t type, uint16_t flags,
                             uint32_t request_id, uint32_t stream_id,
                             const uint8_t *payload, size_t payload_size)
{
    if (runtime->tx_wire_length != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_iris_wire_header_t header = {
        .channel = channel,
        .type = type,
        .flags = flags,
        .session_id = runtime->session_id,
        .request_id = request_id,
        .stream_id = stream_id,
        .sequence = ++runtime->sequence[channel],
        .payload_size = payload_size,
    };
    size_t wire_size = 0;
    esp_err_t err = iris_frame_encode(runtime->tx_wire,
                                      sizeof(runtime->tx_wire), &header,
                                      payload, payload_size, &wire_size);
    if (err == ESP_OK) {
        runtime->tx_wire_length = wire_size;
        runtime->tx_wire_offset = 0;
    }
    return err;
}

static esp_err_t queue_error(iris_runtime_t *runtime, uint32_t request_id,
                             esp_err_t code, uint8_t channel, uint8_t type)
{
    uint8_t payload[8] = {0};
    iris_put_le32(payload, (uint32_t)code);
    payload[4] = channel;
    payload[5] = type;
    return queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                       ESP_IRIS_CONTROL_ERROR,
                       ESP_IRIS_FLAG_RESPONSE | ESP_IRIS_FLAG_ERROR,
                       request_id, 0, payload, sizeof(payload));
}

esp_err_t iris_queue_frame(iris_runtime_t *runtime, uint8_t channel,
                           uint8_t type, uint16_t flags,
                           uint32_t request_id, uint32_t stream_id,
                           const uint8_t *payload, size_t payload_size)
{
    return queue_frame(runtime, channel, type, flags, request_id, stream_id,
                       payload, payload_size);
}

esp_err_t iris_queue_error(iris_runtime_t *runtime, uint32_t request_id,
                           esp_err_t code, uint8_t channel, uint8_t type)
{
    return queue_error(runtime, request_id, code, channel, type);
}

static esp_err_t queue_hello(iris_runtime_t *runtime)
{
    uint8_t payload[256];
    tlv_writer_t writer = {.data = payload, .capacity = sizeof(payload)};
    const esp_app_desc_t *app = esp_app_get_description();
    const uint64_t capabilities = ESP_IRIS_CAP_LOG | ESP_IRIS_CAP_EVENT |
                                  ESP_IRIS_CAP_STATUS |
                                  ESP_IRIS_CAP_TIME_SYNC |
                                  ESP_IRIS_CAP_CRASH |
                                  iris_services_capabilities();
    const uint8_t transport = (uint8_t)iris_transport_kind();
    const uint8_t auth_mode = iris_services_auth_mode();
    const uint32_t reset_reason = (uint32_t)esp_reset_reason();

    if (!tlv_put(&writer, ESP_IRIS_TLV_DEVICE_ID, runtime->device_id,
                 sizeof(runtime->device_id)) ||
            !tlv_put_u64(&writer, ESP_IRIS_TLV_BOOT_ID, runtime->boot_id) ||
            !tlv_put_u64(&writer, ESP_IRIS_TLV_UPTIME_US,
                         (uint64_t)esp_timer_get_time()) ||
            !tlv_put_u16(&writer, ESP_IRIS_TLV_PROTOCOL_VERSION,
                         ESP_IRIS_PROTOCOL_VERSION) ||
            !tlv_put_u64(&writer, ESP_IRIS_TLV_CAPABILITIES, capabilities) ||
            !tlv_put_u8(&writer, ESP_IRIS_TLV_TRANSPORT, transport) ||
            !tlv_put_u8(&writer, ESP_IRIS_TLV_AUTH_MODE, auth_mode) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_RESET_REASON, reset_reason) ||
            !tlv_put_string(&writer, ESP_IRIS_TLV_PROJECT_NAME,
                            app->project_name, sizeof(app->project_name)) ||
            !tlv_put_string(&writer, ESP_IRIS_TLV_APP_VERSION,
                            app->version, sizeof(app->version)) ||
            !tlv_put(&writer, ESP_IRIS_TLV_FIRMWARE_SHA256,
                     app->app_elf_sha256, sizeof(app->app_elf_sha256)) ||
            !tlv_put_string(&writer, ESP_IRIS_TLV_IDF_VERSION,
                            app->idf_ver, sizeof(app->idf_ver)) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_MAX_PAYLOAD,
                         ESP_IRIS_MAX_PAYLOAD_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t challenge_size = 0;
    const uint8_t *challenge =
        iris_services_auth_challenge(&challenge_size);
    if (challenge_size > 0 &&
        !tlv_put(&writer, ESP_IRIS_TLV_AUTH_CHALLENGE,
                 challenge, challenge_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                       ESP_IRIS_CONTROL_HELLO, ESP_IRIS_FLAG_RELIABLE,
                       0, 0, payload, writer.length);
}

static void schedule_event(iris_runtime_t *runtime, uint8_t type)
{
    if (type == 0 || type >= 32) {
        return;
    }
    taskENTER_CRITICAL(&runtime->event_lock);
    runtime->pending_events |= IRIS_EVENT_BIT(type);
    taskEXIT_CRITICAL(&runtime->event_lock);
    if (runtime->task != NULL) {
        xTaskNotifyGive(runtime->task);
    }
}

static void schedule_session_events(iris_runtime_t *runtime)
{
    schedule_event(runtime, ESP_IRIS_EVENT_BOOT);
    schedule_event(runtime, ESP_IRIS_EVENT_LINK_READY);
    if (runtime->previous_boot_crash) {
        schedule_event(runtime, ESP_IRIS_EVENT_PREVIOUS_BOOT_CRASH);
    }
    if (runtime->core_dump_present) {
        schedule_event(runtime, ESP_IRIS_EVENT_CORE_DUMP_AVAILABLE);
    }
    if (runtime->services_ready) {
        schedule_event(runtime, ESP_IRIS_EVENT_SERVICES_READY);
    }
    if (runtime->healthy) {
        schedule_event(runtime, ESP_IRIS_EVENT_HEALTHY);
    }
}

static uint8_t next_pending_event(iris_runtime_t *runtime)
{
    static const uint8_t order[] = {
        ESP_IRIS_EVENT_BOOT,
        ESP_IRIS_EVENT_LINK_READY,
        ESP_IRIS_EVENT_PREVIOUS_BOOT_CRASH,
        ESP_IRIS_EVENT_CORE_DUMP_AVAILABLE,
        ESP_IRIS_EVENT_SERVICES_READY,
        ESP_IRIS_EVENT_HEALTHY,
        ESP_IRIS_EVENT_PLANNED_RESTART,
        ESP_IRIS_EVENT_RECOVERY_ENTERED,
    };
    taskENTER_CRITICAL(&runtime->event_lock);
    const uint32_t pending = runtime->pending_events;
    taskEXIT_CRITICAL(&runtime->event_lock);
    for (size_t i = 0; i < sizeof(order); ++i) {
        if ((pending & IRIS_EVENT_BIT(order[i])) != 0) {
            return order[i];
        }
    }
    return 0;
}

static esp_err_t queue_event(iris_runtime_t *runtime, uint8_t event_type)
{
    uint8_t payload[160];
    tlv_writer_t writer = {.data = payload, .capacity = sizeof(payload)};
    if (!tlv_put(&writer, ESP_IRIS_TLV_DEVICE_ID, runtime->device_id,
                 sizeof(runtime->device_id)) ||
            !tlv_put_u64(&writer, ESP_IRIS_TLV_BOOT_ID, runtime->boot_id) ||
            !tlv_put_u64(&writer, ESP_IRIS_TLV_UPTIME_US,
                         (uint64_t)esp_timer_get_time()) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_RESET_REASON,
                         (uint32_t)esp_reset_reason()) ||
            (event_type == ESP_IRIS_EVENT_CORE_DUMP_AVAILABLE &&
             !tlv_put_u32(&writer, ESP_IRIS_TLV_CORE_DUMP_SIZE,
                          runtime->core_dump_size <= UINT32_MAX
                            ? (uint32_t)runtime->core_dump_size
                            : UINT32_MAX))) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = queue_frame(runtime, ESP_IRIS_CHANNEL_EVENT,
                                event_type, ESP_IRIS_FLAG_RELIABLE,
                                0, 0, payload, writer.length);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&runtime->event_lock);
        runtime->pending_events &= ~IRIS_EVENT_BIT(event_type);
        taskEXIT_CRITICAL(&runtime->event_lock);
    }
    return err;
}

static esp_err_t queue_status(iris_runtime_t *runtime, uint32_t request_id)
{
    uint8_t payload[256];
    tlv_writer_t writer = {.data = payload, .capacity = sizeof(payload)};
    uint32_t dropped;
    taskENTER_CRITICAL(&runtime->log_lock);
    dropped = runtime->log_dropped_bytes;
    taskEXIT_CRITICAL(&runtime->log_lock);

    if (!tlv_put_u64(&writer, ESP_IRIS_TLV_UPTIME_US,
                     (uint64_t)esp_timer_get_time()) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_FREE_INTERNAL,
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_MIN_FREE_INTERNAL,
                         heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_LOG_DROPPED, dropped) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_RX_FRAMES,
                         runtime->rx_frames) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_TX_FRAMES,
                         runtime->tx_frames) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_INVALID_FRAMES,
                         runtime->invalid_frames) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_LINK_COUNT,
                         runtime->link_count) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_TASK_STACK_FREE_MIN,
                         runtime->task_stack_free_min_bytes) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_WORKER_ACTIVE_MAX_US,
                         runtime->worker_active_max_us) ||
            !tlv_put_u8(&writer, ESP_IRIS_TLV_LIFECYCLE_STATE,
                        (uint8_t)runtime->lifecycle) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_INTERNAL_HEAP_USED,
                         runtime->internal_heap_used_bytes +
                         (iris_services_allocated_bytes() >
                          runtime->service_bytes_at_start
                          ? iris_services_allocated_bytes() -
                            runtime->service_bytes_at_start : 0)) ||
            !tlv_put_u32(&writer, ESP_IRIS_TLV_STATIC_INTERNAL_BYTES,
                         (uint32_t)(sizeof(*runtime) +
                                    IRIS_STDIO_STATIC_BYTES +
                                    iris_services_static_bytes()))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                       ESP_IRIS_CONTROL_STATUS_RESPONSE,
                       ESP_IRIS_FLAG_RESPONSE, request_id, 0,
                       payload, writer.length);
}

static void handle_control(iris_runtime_t *runtime,
                           const iris_decoded_frame_t *frame,
                           uint64_t received_us)
{
    const esp_iris_wire_header_t *header = &frame->header;
    switch (header->type) {
    case ESP_IRIS_CONTROL_HELLO_ACK:
        if (runtime->hello_acked) {
            break;
        }
        {
            esp_err_t auth_err = iris_services_authenticate(
                runtime, frame->payload, header->payload_size);
            if (iris_services_auth_mode() != 0) {
                const uint8_t result = auth_err == ESP_OK ? 1U : 0U;
                (void)queue_frame(
                    runtime, ESP_IRIS_CHANNEL_CONTROL,
                    ESP_IRIS_CONTROL_AUTH_RESULT,
                    ESP_IRIS_FLAG_RESPONSE |
                        (auth_err == ESP_OK ? 0 : ESP_IRIS_FLAG_ERROR),
                    header->request_id, 0, &result, sizeof(result));
            }
            if (auth_err == ESP_OK) {
                schedule_session_events(runtime);
                (void)transition_session(
                    runtime, IRIS_SESSION_EVENT_AUTHENTICATED);
            }
        }
        break;
    case ESP_IRIS_CONTROL_PING:
        (void)queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                          ESP_IRIS_CONTROL_PONG, ESP_IRIS_FLAG_RESPONSE,
                          header->request_id, 0, frame->payload,
                          header->payload_size);
        break;
    case ESP_IRIS_CONTROL_TIME_SYNC_REQUEST:
        if (header->payload_size == 8U) {
            uint8_t payload[24];
            memcpy(payload, frame->payload, 8);
            iris_put_le64(payload + 8, received_us);
            iris_put_le64(payload + 16, (uint64_t)esp_timer_get_time());
            (void)queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                              ESP_IRIS_CONTROL_TIME_SYNC_RESPONSE,
                              ESP_IRIS_FLAG_RESPONSE, header->request_id, 0,
                              payload, sizeof(payload));
        } else {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel,
                              header->type);
        }
        break;
    case ESP_IRIS_CONTROL_STATUS_REQUEST:
        (void)queue_status(runtime, header->request_id);
        break;
    case ESP_IRIS_CONTROL_CREDIT:
        if (header->payload_size == 8U &&
                frame->payload[0] == ESP_IRIS_CHANNEL_LOG) {
            const uint32_t amount = iris_get_le32(frame->payload + 4);
            runtime->log_credit = UINT32_MAX - runtime->log_credit < amount
                ? UINT32_MAX : runtime->log_credit + amount;
        } else if (header->payload_size == 8U &&
                   iris_services_credit(frame->payload[0],
                                        iris_get_le32(frame->payload + 4))) {
            /* Media credits are maintained independently per channel. */
        } else {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_ARG, header->channel,
                              header->type);
        }
        break;
    default:
        (void)queue_error(runtime, header->request_id,
                          ESP_ERR_NOT_SUPPORTED, header->channel,
                          header->type);
        break;
    }
}

static void handle_crash(iris_runtime_t *runtime,
                         const iris_decoded_frame_t *frame)
{
    const esp_iris_wire_header_t *header = &frame->header;
    if (header->type == ESP_IRIS_CRASH_METADATA_REQUEST) {
        if (header->payload_size != 0) {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel,
                              header->type);
            return;
        }
        size_t payload_size = 0;
        esp_err_t err = iris_crash_build_metadata(
            runtime, runtime->rx_wire, sizeof(runtime->rx_wire),
            &payload_size);
        if (err == ESP_OK) {
            err = queue_frame(runtime, ESP_IRIS_CHANNEL_CRASH,
                              ESP_IRIS_CRASH_METADATA_RESPONSE,
                              ESP_IRIS_FLAG_RESPONSE, header->request_id, 0,
                              runtime->rx_wire, payload_size);
        }
        if (err != ESP_OK) {
            (void)queue_error(runtime, header->request_id, err,
                              header->channel, header->type);
        }
        return;
    }
    if (header->type == ESP_IRIS_CRASH_READ_REQUEST) {
        if (header->payload_size != 8U) {
            (void)queue_error(runtime, header->request_id,
                              ESP_ERR_INVALID_SIZE, header->channel,
                              header->type);
            return;
        }
        const uint32_t offset = iris_get_le32(frame->payload);
        const uint16_t maximum = iris_get_le16(frame->payload + 4);
        size_t chunk_size = 0;
        esp_err_t err = iris_crash_read(runtime, offset, maximum,
                                        runtime->rx_wire + 8,
                                        &chunk_size);
        if (err == ESP_OK) {
            iris_put_le32(runtime->rx_wire, offset);
            iris_put_le32(runtime->rx_wire + 4,
                          runtime->core_dump_size <= UINT32_MAX
                            ? (uint32_t)runtime->core_dump_size
                            : UINT32_MAX);
            const bool finished = offset + chunk_size >=
                                  runtime->core_dump_size;
            err = queue_frame(runtime, ESP_IRIS_CHANNEL_CRASH,
                              ESP_IRIS_CRASH_READ_RESPONSE,
                              ESP_IRIS_FLAG_RESPONSE |
                                (finished ? ESP_IRIS_FLAG_STREAM_END : 0),
                              header->request_id, 1,
                              runtime->rx_wire, chunk_size + 8U);
        }
        if (err != ESP_OK) {
            (void)queue_error(runtime, header->request_id, err,
                              header->channel, header->type);
        }
        return;
    }
    (void)queue_error(runtime, header->request_id, ESP_ERR_NOT_SUPPORTED,
                      header->channel, header->type);
}

static void handle_frame(iris_runtime_t *runtime,
                         const iris_decoded_frame_t *frame,
                         uint64_t received_us)
{
    ++runtime->rx_frames;
    if (frame->header.session_id != runtime->session_id) {
        ++runtime->invalid_frames;
        return;
    }
    if (!runtime->hello_acked &&
        !(frame->header.channel == ESP_IRIS_CHANNEL_CONTROL &&
          frame->header.type == ESP_IRIS_CONTROL_HELLO_ACK)) {
        ++runtime->invalid_frames;
        return;
    }
    if (frame->header.channel == ESP_IRIS_CHANNEL_CONTROL &&
        (frame->header.type == ESP_IRIS_CONTROL_HELLO_ACK ||
         frame->header.type == ESP_IRIS_CONTROL_PING ||
         frame->header.type == ESP_IRIS_CONTROL_TIME_SYNC_REQUEST ||
         frame->header.type == ESP_IRIS_CONTROL_STATUS_REQUEST ||
         frame->header.type == ESP_IRIS_CONTROL_CREDIT)) {
        handle_control(runtime, frame, received_us);
    } else if (frame->header.channel == ESP_IRIS_CHANNEL_CRASH) {
        handle_crash(runtime, frame);
    } else if (iris_services_handle_frame(runtime, frame, received_us)) {
        return;
    } else {
        (void)queue_error(runtime, frame->header.request_id,
                          ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                          frame->header.type);
    }
}

static void feed_rx(iris_runtime_t *runtime, const uint8_t *data,
                    size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        const uint8_t value = data[i];
        if (value == 0) {
            if (runtime->rx_discarding) {
                runtime->rx_discarding = false;
                runtime->rx_wire_length = 0;
                continue;
            }
            if (runtime->rx_wire_length == 0) {
                continue;
            }
            iris_decoded_frame_t frame;
            const uint64_t received_us = (uint64_t)esp_timer_get_time();
            if (iris_frame_decode_in_place(runtime->rx_wire,
                                           runtime->rx_wire_length,
                                           &frame) == ESP_OK) {
                handle_frame(runtime, &frame, received_us);
            } else {
                ++runtime->invalid_frames;
            }
            runtime->rx_wire_length = 0;
            continue;
        }
        if (runtime->rx_discarding) {
            continue;
        }
        if (runtime->rx_wire_length >= sizeof(runtime->rx_wire) - 1U) {
            runtime->rx_discarding = true;
            runtime->rx_wire_length = 0;
            ++runtime->invalid_frames;
            continue;
        }
        runtime->rx_wire[runtime->rx_wire_length++] = value;
    }
}

static void begin_session(iris_runtime_t *runtime)
{
    if (!transition_session(runtime, IRIS_SESSION_EVENT_LINK_UP)) {
        return;
    }
    do {
        runtime->session_id = esp_random();
    } while (runtime->session_id == 0);
    memset(runtime->sequence, 0, sizeof(runtime->sequence));
    taskENTER_CRITICAL(&runtime->event_lock);
    runtime->pending_events = 0;
    taskEXIT_CRITICAL(&runtime->event_lock);
    runtime->log_credit = 0;
    runtime->next_hello_us = 0;
    runtime->rx_wire_length = 0;
    runtime->rx_discarding = false;
    runtime->tx_wire_length = 0;
    runtime->tx_wire_offset = 0;
    ++runtime->link_count;
    iris_services_session_begin(runtime);
}

static void end_session(iris_runtime_t *runtime)
{
    iris_services_session_end(runtime);
    (void)transition_session(runtime, IRIS_SESSION_EVENT_LINK_DOWN);
    taskENTER_CRITICAL(&runtime->event_lock);
    runtime->pending_events = 0;
    taskEXIT_CRITICAL(&runtime->event_lock);
    runtime->session_id = 0;
    runtime->log_credit = 0;
    runtime->rx_wire_length = 0;
    runtime->rx_discarding = false;
    runtime->tx_wire_length = 0;
    runtime->tx_wire_offset = 0;
}

static bool flush_tx(iris_runtime_t *runtime)
{
    if (runtime->tx_wire_length == 0) {
        return false;
    }
    const int sent = iris_transport_write(
        runtime, runtime->tx_wire + runtime->tx_wire_offset,
        runtime->tx_wire_length - runtime->tx_wire_offset);
    if (sent <= 0) {
        return false;
    }
    runtime->tx_wire_offset += (size_t)sent;
    if (runtime->tx_wire_offset == runtime->tx_wire_length) {
        runtime->tx_wire_length = 0;
        runtime->tx_wire_offset = 0;
        ++runtime->tx_frames;
    }
    return true;
}

static void queue_next_log(iris_runtime_t *runtime)
{
    if (runtime->log_credit < IRIS_LOG_PAYLOAD_HEADER_SIZE) {
        return;
    }
    iris_log_record_t record;
    if (!iris_log_pop(runtime, runtime->log_credit, &record)) {
        return;
    }
    uint8_t payload[IRIS_LOG_PAYLOAD_HEADER_SIZE + IRIS_LOG_RECORD_DATA_MAX];
    iris_put_le64(payload, record.monotonic_us);
    iris_put_le32(payload + 8, record.dropped_total);
    payload[12] = record.source;
    payload[13] = record.flags;
    iris_put_le16(payload + 14, record.length);
    memcpy(payload + IRIS_LOG_PAYLOAD_HEADER_SIZE, record.data, record.length);
    const size_t payload_size = IRIS_LOG_PAYLOAD_HEADER_SIZE + record.length;
    if (queue_frame(runtime, ESP_IRIS_CHANNEL_LOG, ESP_IRIS_LOG_RECORD, 0,
                    0, 0, payload, payload_size) == ESP_OK) {
        runtime->log_credit -= payload_size;
    }
}

static bool pump_link(iris_runtime_t *runtime, uint8_t *input,
                      size_t input_capacity)
{
    bool progressed = flush_tx(runtime);
    if (runtime->tx_wire_length != 0) {
        iris_services_poll(runtime);
        return progressed;
    }

    const int received = iris_transport_read(runtime, input, input_capacity);
    if (received > 0) {
        feed_rx(runtime, input, (size_t)received);
        progressed = true;
    }

    if (runtime->tx_wire_length == 0) {
        const int64_t now = esp_timer_get_time();
        if (!runtime->hello_acked && now >= runtime->next_hello_us) {
            if (queue_hello(runtime) == ESP_OK) {
                runtime->next_hello_us = now + IRIS_HELLO_INTERVAL_US;
            }
        } else if (runtime->hello_acked &&
                   next_pending_event(runtime) != 0) {
            (void)queue_event(runtime, next_pending_event(runtime));
        } else if (runtime->hello_acked &&
                   iris_services_queue_next(runtime)) {
            /* A service event or media chunk now owns TX. */
        } else if (runtime->hello_acked) {
            queue_next_log(runtime);
        }
    }

    if (runtime->tx_wire_length != 0) {
        progressed = true;
        (void)flush_tx(runtime);
    }
    iris_services_poll(runtime);
    return progressed;
}

static void iris_worker(void *argument)
{
    iris_runtime_t *runtime = argument;
    uint8_t input[256];
    while (runtime->running) {
        const int64_t active_start_us = esp_timer_get_time();
        const iris_link_event_t event = iris_transport_poll(runtime);
        if (event == IRIS_LINK_EVENT_CONNECTED) {
            begin_session(runtime);
        } else if (event == IRIS_LINK_EVENT_DISCONNECTED) {
            end_session(runtime);
        }
        bool progressed = false;
        if (runtime->link_connected) {
            /* Fill the enlarged TinyUSB FIFO in bounded bursts. The limit
             * preserves CONTROL/EVENT responsiveness and prevents a mirror
             * stream from monopolizing this task. */
            for (size_t burst = 0; burst < 8; ++burst) {
                const bool step = pump_link(runtime, input, sizeof(input));
                progressed = progressed || step;
                if (!step || runtime->tx_wire_length != 0) {
                    break;
                }
            }
        }
        const int64_t active_time_us = esp_timer_get_time() - active_start_us;
        if (active_time_us > 0 &&
                (uint64_t)active_time_us > runtime->worker_active_max_us) {
            runtime->worker_active_max_us = active_time_us > UINT32_MAX
                ? UINT32_MAX : (uint32_t)active_time_us;
        }
        const uint32_t stack_free = (uint32_t)uxTaskGetStackHighWaterMark2(NULL);
        if (stack_free < runtime->task_stack_free_min_bytes) {
            runtime->task_stack_free_min_bytes = stack_free;
        }
        if (runtime->link_connected &&
            (progressed || runtime->tx_wire_length != 0)) {
            /* At a 100 Hz tick rate, pdMS_TO_TICKS(1) is zero and does not
             * yield. Yield explicitly so TinyUSB can drain the FIFO between
             * bursts without restoring the old 10 ms per-chunk delay. */
            (void)ulTaskNotifyTake(pdTRUE, 0);
            taskYIELD();
        } else {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        }
    }
    end_session(runtime);
    iris_transport_stop(runtime);
    runtime->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp_iris_start(void)
{
    taskENTER_CRITICAL(&s_start_lock);
    if (g_iris.started) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_OK;
    }
    if (g_iris.initializing) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_iris.initializing = true;
    if (!transition_lifecycle(&g_iris, ESP_IRIS_LIFECYCLE_STARTING)) {
        g_iris.initializing = false;
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_start_lock);

    const uint32_t services_before = iris_services_allocated_bytes();
    const uint32_t heap_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) + services_before;
    esp_err_t err = ESP_OK;
    if (!g_iris.identity_ready) {
        err = iris_identity_load_or_create(&g_iris);
        if (err == ESP_OK) {
            g_iris.identity_ready = true;
        }
    }
    if (err == ESP_OK && !g_iris.crash_initialized) {
        iris_crash_probe(&g_iris);
    }
    if (err == ESP_OK) {
        err = iris_services_init(&g_iris);
    }
    if (err == ESP_OK) {
        err = iris_transport_start(&g_iris);
    }
    if (err == ESP_OK) {
        err = iris_log_vfs_init(&g_iris);
        g_iris.vfs_registered = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = iris_log_redirect_stdio();
        g_iris.stdio_redirected = err == ESP_OK;
    }
    if (err == ESP_OK) {
        g_iris.running = true;
        g_iris.task_stack_free_min_bytes = UINT32_MAX;
        if (xTaskCreate(iris_worker, "esp_iris",
                        CONFIG_ESP_IRIS_TASK_STACK_SIZE, &g_iris,
                        CONFIG_ESP_IRIS_TASK_PRIORITY, &g_iris.task) != pdPASS) {
            g_iris.running = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        iris_services_deinit(&g_iris);
        if (g_iris.stdio_redirected) {
            (void)iris_log_restore_stdio();
            g_iris.stdio_redirected = false;
        }
        if (g_iris.vfs_registered) {
            (void)iris_log_vfs_deinit();
            g_iris.vfs_registered = false;
        }
        iris_transport_stop(&g_iris);
    }

    taskENTER_CRITICAL(&s_start_lock);
    g_iris.started = err == ESP_OK;
    g_iris.initializing = false;
    (void)transition_lifecycle(
        &g_iris, err == ESP_OK ? ESP_IRIS_LIFECYCLE_RUNNING
                               : ESP_IRIS_LIFECYCLE_FAILED);
    taskEXIT_CRITICAL(&s_start_lock);
    if (err == ESP_OK) {
        const uint32_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        g_iris.internal_heap_used_bytes = heap_before >= heap_after
            ? heap_before - heap_after : 0;
        g_iris.service_bytes_at_start = iris_services_allocated_bytes();
    }
    return err;
}

esp_err_t esp_iris_stop(void)
{
    taskENTER_CRITICAL(&s_start_lock);
    if (g_iris.lifecycle == ESP_IRIS_LIFECYCLE_STOPPED) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_OK;
    }
    if (g_iris.initializing ||
            g_iris.lifecycle == ESP_IRIS_LIFECYCLE_STOPPING) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    TaskHandle_t worker = g_iris.task;
    if (worker != NULL && worker == xTaskGetCurrentTaskHandle()) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_iris.started = false;
    g_iris.running = false;
    if (!transition_lifecycle(&g_iris, ESP_IRIS_LIFECYCLE_STOPPING)) {
        taskEXIT_CRITICAL(&s_start_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_start_lock);

    if (worker != NULL) {
        xTaskNotifyGive(worker);
        const TickType_t deadline = xTaskGetTickCount() +
            pdMS_TO_TICKS(IRIS_STOP_TIMEOUT_MS);
        while (g_iris.task != NULL &&
               (int32_t)(deadline - xTaskGetTickCount()) > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (g_iris.task != NULL) {
            (void)transition_lifecycle(&g_iris,
                                       ESP_IRIS_LIFECYCLE_FAILED);
            return ESP_ERR_TIMEOUT;
        }
    } else {
        iris_transport_stop(&g_iris);
    }
    iris_services_deinit(&g_iris);

    esp_err_t result = ESP_OK;
    if (g_iris.stdio_redirected) {
        result = iris_log_restore_stdio();
        g_iris.stdio_redirected = false;
    }
    if (g_iris.vfs_registered) {
        esp_err_t vfs_err = iris_log_vfs_deinit();
        if (result == ESP_OK) {
            result = vfs_err;
        }
        g_iris.vfs_registered = false;
    }
    taskENTER_CRITICAL(&g_iris.log_lock);
    g_iris.log_head = 0;
    g_iris.log_tail = 0;
    g_iris.log_used = 0;
    taskEXIT_CRITICAL(&g_iris.log_lock);
    (void)transition_lifecycle(
        &g_iris, result == ESP_OK ? ESP_IRIS_LIFECYCLE_STOPPED
                                  : ESP_IRIS_LIFECYCLE_FAILED);
    return result;
}

bool esp_iris_is_started(void)
{
    return g_iris.started;
}

esp_err_t esp_iris_get_status(esp_iris_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_iris.identity_ready) {
        memset(out_status, 0, sizeof(*out_status));
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t dropped;
    taskENTER_CRITICAL(&g_iris.log_lock);
    dropped = g_iris.log_dropped_bytes;
    taskEXIT_CRITICAL(&g_iris.log_lock);
    *out_status = (esp_iris_status_t) {
        .started = g_iris.started,
        .link_connected = g_iris.link_connected,
        .session_ready = g_iris.hello_acked,
        .previous_boot_crash = g_iris.previous_boot_crash,
        .core_dump_present = g_iris.core_dump_present,
        .core_dump_valid = g_iris.core_dump_valid,
        .lifecycle = g_iris.lifecycle,
        .transport = iris_transport_kind(),
        .boot_id = g_iris.boot_id,
        .session_id = g_iris.session_id,
        .uptime_us = (uint64_t)esp_timer_get_time(),
        .rx_frames = g_iris.rx_frames,
        .tx_frames = g_iris.tx_frames,
        .invalid_frames = g_iris.invalid_frames,
        .link_count = g_iris.link_count,
        .log_dropped_bytes = dropped,
        .task_stack_free_min_bytes = g_iris.task_stack_free_min_bytes,
        .worker_active_max_us = g_iris.worker_active_max_us,
        .internal_heap_used_bytes = g_iris.internal_heap_used_bytes +
            (iris_services_allocated_bytes() > g_iris.service_bytes_at_start
                ? iris_services_allocated_bytes() -
                  g_iris.service_bytes_at_start : 0),
        .static_internal_bytes = sizeof(g_iris) + IRIS_STDIO_STATIC_BYTES +
                                 iris_services_static_bytes(),
        .core_dump_size = g_iris.core_dump_size <= UINT32_MAX
            ? (uint32_t)g_iris.core_dump_size : UINT32_MAX,
        .reset_reason = (uint32_t)esp_reset_reason(),
    };
    memcpy(out_status->device_id, g_iris.device_id,
           sizeof(out_status->device_id));
    return ESP_OK;
}

esp_err_t esp_iris_mark_services_ready(void)
{
    if (!g_iris.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_iris.services_ready) {
        g_iris.services_ready = true;
        if (g_iris.hello_acked) {
            schedule_event(&g_iris, ESP_IRIS_EVENT_SERVICES_READY);
        }
    }
    return ESP_OK;
}

esp_err_t esp_iris_mark_planned_restart(void)
{
    if (!g_iris.started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_iris_platform_mark_planned_restart();
    if ((err == ESP_OK || err == ESP_ERR_NOT_SUPPORTED) &&
        g_iris.hello_acked) {
        schedule_event(&g_iris, ESP_IRIS_EVENT_PLANNED_RESTART);
    }
    return err;
}

esp_err_t esp_iris_mark_healthy(void)
{
    if (!g_iris.started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_iris_platform_mark_healthy();
    if (err == ESP_OK) {
        g_iris.healthy = true;
        if (g_iris.hello_acked) {
            schedule_event(&g_iris, ESP_IRIS_EVENT_HEALTHY);
        }
    }
    return err;
}

esp_err_t esp_iris_format_device_id(char out[33])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_iris.started && !g_iris.initializing) {
        out[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(g_iris.device_id); ++i) {
        out[i * 2] = hex[g_iris.device_id[i] >> 4];
        out[i * 2 + 1] = hex[g_iris.device_id[i] & 0x0f];
    }
    out[32] = '\0';
    return ESP_OK;
}
