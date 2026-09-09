#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/esp_iris.c"
#include "../../../src/esp_iris_services.c"
#include "../../../src/esp_iris_system_inventory.c"
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE
#include "../../../src/esp_iris_system_update.c"
#endif
#include "../../../src/esp_iris_transport.c"
#include "runtime_stubs.h"
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
void *heap_caps_calloc(size_t a, size_t b, unsigned caps) { (void)caps; return calloc(a,b); }
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
uint32_t esp_random(void) { static uint32_t id = 50; return ++id; }
void esp_fill_random(void *p, size_t n) { memset(p, 0x5a, n); }
static size_t reported_size;
static unsigned callback_calls;
static esp_err_t callback_result;
static void (*during_callback)(void);
static esp_err_t callback(const esp_iris_rpc_request_t *r, uint8_t *out,
    size_t cap, size_t *size, void *ctx) {
    (void)r; (void)ctx; ++callback_calls; memset(out, 0xa5, cap);
    *size = reported_size; if (during_callback) during_callback(); return callback_result;
}
static void test_rpc_lengths(void) {
    iris_runtime_t rt = {0};
    iris_service_state_t state = { .magic = IRIS_SERVICE_STATE_MAGIC };
    s_services = &state;
    state.rpc[0] = (iris_rpc_entry_t){true, 1, 1, callback, NULL};
    uint8_t payload[12] = {1,0,1,0};
    iris_decoded_frame_t request = {.header = {
        .channel = ESP_IRIS_CHANNEL_CONTROL, .type = ESP_IRIS_CONTROL_REQUEST,
        .payload_size = sizeof(payload)}, .payload = payload};
    const size_t sizes[] = {0, CONFIG_ESP_IRIS_RPC_BODY_BYTES,
        CONFIG_ESP_IRIS_RPC_BODY_BYTES + 1, SIZE_MAX};
    for (unsigned error = 0; error < 2; ++error) {
        callback_result = error ? ESP_FAIL : ESP_OK;
        for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
            reported_size = sizes[i]; ++request.header.request_id;
            rt.tx_wire_length = 0;
            assert(handle_rpc(&rt, &request, 0));
            iris_decoded_frame_t response;
            assert(iris_frame_decode_in_place(rt.tx_wire,
                rt.tx_wire_length - 1, &response) == ESP_OK);
            bool oversized = reported_size > CONFIG_ESP_IRIS_RPC_BODY_BYTES;
            assert(response.header.payload_size == 12 + (oversized ? 0 : reported_size));
            assert((esp_err_t)iris_get_le32(response.payload + 4) ==
                (oversized ? ESP_ERR_INVALID_SIZE : callback_result));
        }
    }
    s_services = NULL;
}
static void append_control(uint8_t type, uint32_t request_id, const uint8_t *payload, size_t n) {
    esp_iris_wire_header_t h = {.channel = ESP_IRIS_CHANNEL_CONTROL,
        .type = type, .session_id = 123, .request_id = request_id,
        .sequence = request_id, .payload_size = n};
    size_t encoded;
    assert(iris_frame_encode(incoming + incoming_size, sizeof(incoming) - incoming_size,
        &h, payload, n, &encoded) == ESP_OK);
    incoming_size += encoded;
}
static void test_coalesced_frames(void) {
    incoming_size = 0;
    append_control(ESP_IRIS_CONTROL_PING, 1, NULL, 0);
    incoming[incoming_size++] = 0x99; incoming[incoming_size++] = 0;
    uint8_t credit[8] = {ESP_IRIS_CHANNEL_LOG, 0, 0, 0, 5, 0, 0, 0};
    append_control(ESP_IRIS_CONTROL_CREDIT, 2, credit, sizeof(credit));
    append_control(ESP_IRIS_CONTROL_PING, 3, NULL, 0);
    append_control(ESP_IRIS_CONTROL_PING, 4, NULL, 0);
    for (size_t segment = 1; segment <= incoming_size; ++segment) {
        for (size_t partial = 1; partial <= 40; partial += 13) {
            iris_runtime_t rt = {.session_id = 123, .hello_acked = true,
                .next_hello_us = INT64_MAX};
            rt.transport.active_ops = &g_iris_tcp_transport_ops;
            rt.transport.active_state = &rt.transport.tcp;
            read_limit = segment; incoming_offset = 0; outgoing_size = 0;
            write_limit = 0;
            for (unsigned step = 0; step < 300 && !rt.tx_wire_length; ++step) pump_link(&rt);
            assert(rt.tx_wire_length > 0);
            size_t saved_offset = incoming_offset;
            for (unsigned step = 0; step < 5; ++step) pump_link(&rt);
            assert(incoming_offset == saved_offset && outgoing_size == 0);
            write_limit = partial;
            for (unsigned step = 0; step < 1000; ++step) pump_link(&rt);
            assert(incoming_offset == incoming_size);
            assert(rt.rx_pending_offset == rt.rx_pending_length);
            assert(rt.tx_frames == 3 && rt.invalid_frames == 1);
            size_t start = 0; unsigned replies = 0;
            const uint32_t ids[] = {1,3,4};
            for (size_t i = 0; i < outgoing_size; ++i) if (outgoing[i] == 0) {
                iris_decoded_frame_t response;
                assert(iris_frame_decode_in_place(outgoing + start, i-start, &response) == ESP_OK);
                assert(response.header.type == ESP_IRIS_CONTROL_PONG);
                assert(response.header.request_id == ids[replies++]); start = i + 1;
            }
            assert(replies == 3);
        }
    }
}
static void deliver(iris_runtime_t *runtime, const iris_decoded_frame_t *frame) {
    handle_frame(runtime, frame, 0);
    executor_run_one();
    if (runtime->tx_wire_length == 0) (void)executor_queue_next(runtime);
}
static void test_replay_and_reopen(void) {
    iris_runtime_t rt = { .session_id = 123, .hello_acked = true,
        .session_state = IRIS_SESSION_READY, .link_connected = true };
    iris_service_state_t state = {.magic = IRIS_SERVICE_STATE_MAGIC};
    s_services = &state;
    state.rpc[0] = (iris_rpc_entry_t){true, 1, 1, callback, NULL};
    uint8_t payload[12] = {1,0,1,0};
    iris_decoded_frame_t frame = {.header = {.channel = ESP_IRIS_CHANNEL_CONTROL,
        .type = ESP_IRIS_CONTROL_REQUEST, .payload_size = 12, .session_id = 123},
        .payload = payload};
    callback_calls = 0; reported_size = 0; callback_result = ESP_OK;
    const uint32_t requests[] = {10,11,10,11,0,9,12};
    for (unsigned i = 0; i < sizeof(requests)/sizeof(requests[0]); ++i) {
        frame.header.request_id = requests[i]; frame.header.sequence = i + 1;
        rt.tx_wire_length = 0; deliver(&rt, &frame);
    }
    assert(callback_calls == 3);
    frame.header.request_id = 13; frame.header.sequence = 7;
    rt.tx_wire_length = 0; deliver(&rt, &frame);
    assert(callback_calls == 3 && rt.invalid_frames == 1);
    frame.header.sequence = 6; deliver(&rt, &frame);
    assert(callback_calls == 3 && rt.invalid_frames == 2);
    /* Same ID with changed body/method cannot invoke a different handler. */
    frame.header.sequence = 8; frame.header.request_id = 12; payload[2] = 2;
    deliver(&rt, &frame); assert(callback_calls == 3); payload[2] = 1;
    /* Wrap acceptance and half-space rejection. */
    state.last_rpc_request_id = UINT32_MAX; rt.rx_sequence[0] = UINT32_MAX;
    frame.header.request_id = 1; frame.header.sequence = 0;
    rt.tx_wire_length = 0; deliver(&rt, &frame); assert(callback_calls == 4);
    frame.header.request_id = UINT32_C(0x80000001); frame.header.sequence = 1;
    rt.tx_wire_length = 0; deliver(&rt, &frame); assert(callback_calls == 4);
    /* A negotiated reopen produces a fresh session; delayed ACK of the old
     * session cannot reset it again, and normal ACK replay remains harmless. */
    frame.header.type = ESP_IRIS_CONTROL_HELLO_ACK;
    frame.header.flags = ESP_IRIS_FLAG_NEW_SESSION; frame.header.payload_size = 0;
    rt.tx_wire_length = 0; deliver(&rt, &frame);
    const uint32_t new_session = rt.session_id;
    assert(new_session != 123 && new_session != 0 && !rt.hello_acked);
    assert(!state.rpc_request_seen && !rt.rx_sequence_seen[0]);
    rt.tx_wire_length = 0; deliver(&rt, &frame); assert(rt.session_id == new_session);
    frame.header.session_id = new_session; frame.header.flags = 0;
    deliver(&rt, &frame); assert(rt.hello_acked);
    deliver(&rt, &frame); assert(rt.session_id == new_session && rt.hello_acked);
    s_services = NULL;
}
static void test_claim_timeout(void) {
    iris_runtime_t rt = {0};
    now_us = 0; starts = stops = 0; candidate = false;
    assert(iris_transport_start(&rt) == ESP_OK);
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_NONE);
    candidate = true;
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_CONNECTED);
    assert(!rt.transport.committed);
    now_us = IRIS_CLAIM_TIMEOUT_US - 1;
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_NONE);
    now_us++;
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_DISCONNECTED);
    assert(rt.transport.active_ops == NULL && stops == 1);
    candidate = true;
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_CONNECTED);
    iris_transport_commit(&rt);
    assert(rt.transport.committed);
    now_us += 10 * IRIS_CLAIM_TIMEOUT_US;
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_NONE);
    assert(rt.transport.active_ops != NULL);
    iris_transport_renew_claim(&rt);
    assert(!rt.transport.committed);
    now_us += IRIS_CLAIM_TIMEOUT_US;
    assert(iris_transport_poll(&rt) == IRIS_LINK_EVENT_DISCONNECTED);
    iris_transport_stop(&rt);
}
static iris_runtime_t *responsive_runtime;
static unsigned cancel_calls;
static bool abandon_in_callback;
static void slow_cancel(void *ctx) { ++cancel_calls; }
static void prove_ping_responsive(void) {
    static uint32_t sequence = 100;
    iris_runtime_t *rt = responsive_runtime;
    iris_decoded_frame_t ping = {.header = {.channel = ESP_IRIS_CHANNEL_CONTROL,
        .type = ESP_IRIS_CONTROL_PING, .session_id = rt->session_id,
        .request_id = 900, .sequence = sequence++}};
    rt->tx_wire_length = 0;
    handle_frame(rt, &ping, now_us);
    iris_decoded_frame_t response;
    assert(iris_frame_decode_in_place(rt->tx_wire, rt->tx_wire_length - 1, &response) == ESP_OK);
    assert(response.header.type == ESP_IRIS_CONTROL_PONG);
    rt->tx_wire_length = 0;
}
static void slow_callback_hook(void) {
    prove_ping_responsive();
    assert(esp_iris_rpc_unregister(1, 1) == ESP_ERR_INVALID_STATE);
    if (abandon_in_callback) {
        iris_services_session_end(responsive_runtime);
        return;
    }
    uint8_t payload[4]; iris_put_le32(payload, 1);
    iris_decoded_frame_t cancel = {.header = {.channel = ESP_IRIS_CHANNEL_CONTROL,
        .type = ESP_IRIS_CONTROL_CANCEL, .request_id = 901, .payload_size = 4}, .payload = payload};
    assert(executor_dispatch(responsive_runtime, &cancel, now_us));
    responsive_runtime->tx_wire_length = 0;
    iris_put_le32(payload, 2);
    assert(executor_dispatch(responsive_runtime, &cancel, now_us));
    assert(cancel_calls == 0); /* callback is deferred, acknowledgment is ready */
    responsive_runtime->tx_wire_length = 0;
    now_us = 2000000;
}
static void test_executor_rpc(void) {
    iris_runtime_t rt = {.session_id = 600, .hello_acked = true};
    iris_service_state_t state = {.magic = IRIS_SERVICE_STATE_MAGIC};
    s_services = &state; responsive_runtime = &rt;
    state.rpc[0] = (iris_rpc_entry_t){true, 1, 1, callback, NULL};
    esp_iris_job_handle_t job;
    assert(esp_iris_job_create(1, NULL, NULL, &job) == ESP_OK);
    assert(esp_iris_job_create(1, slow_cancel, NULL, &job) == ESP_OK);
    uint8_t payload[12] = {1,0,1,0}; iris_put_le32(payload + 4, 1000);
    iris_decoded_frame_t request = {.header = {.channel = ESP_IRIS_CHANNEL_CONTROL,
        .type = ESP_IRIS_CONTROL_REQUEST, .request_id = 1, .payload_size = 12}, .payload = payload};
    now_us = 0; reported_size = 0; callback_calls = 0; cancel_calls = 0;
    during_callback = slow_callback_hook; abandon_in_callback = false;
    assert(executor_dispatch(&rt, &request, 0));
    assert(callback_calls == 0 && rt.tx_wire_length == 0 && executor_busy());
    prove_ping_responsive();
    assert(executor_dispatch(&rt, &request, 0)); /* bounded saturation rejection */
    rt.tx_wire_length = 0;
    executor_run_one();
    assert(callback_calls == 1 && cancel_calls == 1);
    rt.tx_wire_length = 1; /* completion must survive TX backpressure */
    assert(!executor_queue_next(&rt) && executor_busy());
    rt.tx_wire_length = 0;
    assert(executor_queue_next(&rt));
    iris_decoded_frame_t response;
    assert(iris_frame_decode_in_place(rt.tx_wire, rt.tx_wire_length - 1, &response) == ESP_OK);
    assert(iris_get_le32(response.payload + 4) == ESP_ERR_TIMEOUT);
    assert(response.header.payload_size == 12); /* side effects are not rolled back */
    during_callback = NULL; rt.tx_wire_length = 0; request.header.request_id++;
    assert(executor_dispatch(&rt, &request, 0)); /* already-expired queued work */
    executor_run_one(); assert(callback_calls == 1); assert(executor_queue_next(&rt));
    now_us = 0; rt.tx_wire_length = 0; request.header.request_id++;
    abandon_in_callback = true; during_callback = slow_callback_hook;
    assert(executor_dispatch(&rt, &request, 0)); executor_run_one();
    assert(!executor_busy() && !executor_queue_next(&rt));
    assert(callback_calls == 2 && !state.rpc_request_seen);
    during_callback = NULL; rt.tx_wire_length = 0; request.header.request_id++;
    assert(executor_dispatch(&rt, &request, 0));
    iris_services_deinit(&rt); /* no unsafe task deletion or in-flight free */
    assert(executor_busy()); executor_run_one();
    assert(callback_calls == 2 && !executor_busy());
    s_services = NULL; responsive_runtime = NULL;
}
#if CONFIG_ESP_IRIS_OTA
static void flash_hook(void) {
    prove_ping_responsive();
    esp_iris_ota_status_t status;
    assert(esp_iris_ota_get_status(&status) == ESP_OK);
    uint8_t no_payload[1] = {0};
    iris_decoded_frame_t cancel = {.header = {.channel = ESP_IRIS_CHANNEL_OTA,
        .type = ESP_IRIS_OTA_CANCEL, .request_id = 950}, .payload = no_payload};
    assert(executor_dispatch(responsive_runtime, &cancel, now_us));
    responsive_runtime->tx_wire_length = 0;
}
static void test_executor_ota(void) {
    iris_runtime_t rt = {.session_id = 700, .hello_acked = true};
    iris_service_state_t state = {.magic = IRIS_SERVICE_STATE_MAGIC};
    s_services = &state; responsive_runtime = &rt;
    uint8_t begin[40] = {4}; uint8_t data[8] = {0,0,0,0,1,2,3,4};
    iris_decoded_frame_t request = {.header = {.channel = ESP_IRIS_CHANNEL_OTA,
        .request_id = 1}, .payload = begin};
    flash_writes = boot_selections = flash_aborts = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        during_flash = NULL; request.header.type = ESP_IRIS_OTA_BEGIN;
        request.header.payload_size = sizeof(begin); request.payload = begin;
        rt.tx_wire_length = 0;
        assert(executor_dispatch(&rt, &request, now_us)); executor_run_one();
        assert(executor_queue_next(&rt) && state.ota.active);
        request.header.type = ESP_IRIS_OTA_DATA; request.header.payload_size = sizeof(data);
        request.payload = data; rt.tx_wire_length = 0;
        during_flash = mode == 0 ? flash_hook : NULL;
        assert(executor_dispatch(&rt, &request, now_us)); executor_run_one();
        assert(executor_queue_next(&rt));
        if (mode == 0) { assert(!state.ota.active && flash_aborts == 1); continue; }
        request.header.type = ESP_IRIS_OTA_END; request.header.payload_size = 0;
        rt.tx_wire_length = 0; during_flash = mode == 1 ? flash_hook : NULL;
        assert(executor_dispatch(&rt, &request, now_us)); executor_run_one();
        assert(executor_queue_next(&rt) && !state.ota.active);
        assert(boot_selections == (mode == 2 ? 1U : 0U));
    }
    assert(flash_writes == 3); during_flash = NULL; s_services = NULL; responsive_runtime = NULL;
}
#endif
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE
static unsigned system_writes, system_commits, system_aborts;
static esp_err_t system_write(const esp_iris_system_update_component_t *c,
    uint32_t offset, const uint8_t *data, size_t size, void *ctx) {
    ++system_writes; prove_ping_responsive();
    assert(esp_iris_system_update_unregister(NULL) == ESP_ERR_INVALID_STATE);
    iris_decoded_frame_t status = {.header = {.channel = ESP_IRIS_CHANNEL_SYSTEM_UPDATE,
        .type = ESP_IRIS_SYSTEM_UPDATE_STATUS, .request_id = 970}};
    assert(executor_dispatch(responsive_runtime, &status, now_us));
    iris_decoded_frame_t response;
    assert(iris_frame_decode_in_place(responsive_runtime->tx_wire,
        responsive_runtime->tx_wire_length - 1, &response) == ESP_OK);
    assert(response.header.type == ESP_IRIS_SYSTEM_UPDATE_STATUS_RESPONSE);
    assert(iris_get_le32(response.payload + 24) == 0); /* completed snapshot */
    responsive_runtime->tx_wire_length = 0;
    uint8_t operation_id[16] = {1};
    assert(iris_system_update_request_cancel(operation_id, 16) == ESP_OK);
    return ESP_OK;
}
static esp_err_t system_commit(const uint8_t *id, void *ctx) {
    ++system_commits; prove_ping_responsive(); return ESP_OK;
}
static void system_abort(const uint8_t *id, esp_err_t reason, void *ctx) { ++system_aborts; }
static void test_executor_system_update(void) {
    iris_runtime_t rt = {.session_id = 900, .hello_acked = true};
    iris_service_state_t state = {.magic = IRIS_SERVICE_STATE_MAGIC};
    s_services = &state; responsive_runtime = &rt;
    s_system_update = (iris_system_update_state_t){.backend_registered = true};
    s_system_update.backend.write_component = system_write;
    s_system_update.backend.commit = system_commit;
    s_system_update.backend.abort = system_abort;
    s_system_update.status.operation_id[0] = 1;
    s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING;
    s_system_update.status.component_count = 1;
    s_system_update.status.component_size = 4;
    s_system_update.component.id = 1; s_system_update.component.size = 4;
    assert(esp_iris_job_create(0x101, job_cancel, NULL, &s_system_update.job) == ESP_OK);
    publish_status();
    uint8_t payload[28] = {1}; payload[16] = 1;
    iris_decoded_frame_t request = {.header = {.channel = ESP_IRIS_CHANNEL_SYSTEM_UPDATE,
        .type = ESP_IRIS_SYSTEM_UPDATE_DATA, .request_id = 1, .payload_size = 28}, .payload = payload};
    assert(executor_dispatch(&rt, &request, now_us));
    assert(system_writes == 0); executor_run_one();
    assert(executor_queue_next(&rt));
    assert(system_writes == 1 && system_aborts == 1);
    assert(s_system_update.status.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED);
    uint8_t bad_operation[16] = {2};
    assert(iris_system_update_request_cancel(bad_operation, 16) == ESP_ERR_INVALID_STATE);
    for (unsigned cancelled = 1; cancelled <= 2; ++cancelled) {
        s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED;
        s_system_update.status.completed_components = 1;
        assert(esp_iris_job_create(0x101, job_cancel, NULL, &s_system_update.job) == ESP_OK);
        publish_status();
        rt.tx_wire_length = 0; request.header.type = ESP_IRIS_SYSTEM_UPDATE_COMMIT;
        request.header.payload_size = 16;
        assert(executor_dispatch(&rt, &request, now_us));
        if (cancelled == 1) assert(iris_system_update_request_cancel(payload, 16) == ESP_OK);
        executor_run_one(); assert(executor_queue_next(&rt));
        assert(system_commits == (cancelled == 1 ? 0U : 1U));
    }
    s_system_update = (iris_system_update_state_t){0};
    s_services = NULL; responsive_runtime = NULL;
}
#endif
#if CONFIG_ESP_IRIS_SYSTEM_INVENTORY
static unsigned inventory_reads;
static esp_err_t inventory_result;
static esp_err_t read_inventory(esp_iris_system_inventory_t *out, void *ctx) {
    ++inventory_reads;
    prove_ping_responsive();
    assert(esp_iris_system_inventory_unregister(NULL) == ESP_ERR_INVALID_STATE);
    out->flags = ESP_IRIS_SYSTEM_INVENTORY_VALID_FLAGS;
    out->layout_version = 17;
    memset(out->bootloader_sha256, 0x55, sizeof(out->bootloader_sha256));
    memset(out->partition_table_sha256, 0x77, sizeof(out->partition_table_sha256));
    memset(out->last_operation_id, 0x99, sizeof(out->last_operation_id));
    out->last_result = ESP_OK;
    return inventory_result;
}
static void test_inventory_executor_dispatch(void) {
    iris_runtime_t rt = {.session_id = 950, .hello_acked = true};
    responsive_runtime = &rt;
    const esp_iris_system_inventory_provider_t provider = {.get_inventory = read_inventory};
    assert(esp_iris_system_inventory_register(&provider) == ESP_OK);
    uint8_t payload[1] = {0};
    iris_decoded_frame_t request = {.header = {.channel = ESP_IRIS_CHANNEL_SYSTEM_UPDATE,
        .type = ESP_IRIS_SYSTEM_UPDATE_INVENTORY, .session_id = 950,
        .request_id = 100, .sequence = 1}, .payload = payload};
    inventory_reads = 0; inventory_result = ESP_OK;
    /* Enter through the production frame router, not a direct provider call. */
    handle_frame(&rt, &request, now_us);
    assert(executor_busy() && inventory_reads == 0);
    executor_run_one(); assert(executor_queue_next(&rt));
    iris_decoded_frame_t response;
    assert(iris_frame_decode_in_place(rt.tx_wire, rt.tx_wire_length - 1, &response) == ESP_OK);
    assert(inventory_reads == 1);
    assert(response.header.type == ESP_IRIS_SYSTEM_UPDATE_INVENTORY_RESPONSE);
    assert(response.header.request_id == 100 && response.header.payload_size == 92);
    assert(iris_get_le32(response.payload) == ESP_IRIS_SYSTEM_INVENTORY_VALID_FLAGS);
    assert(iris_get_le32(response.payload + 4) == 17);
    assert(response.payload[8] == 0x55 && response.payload[40] == 0x77);
    assert(response.payload[72] == 0x99 && iris_get_le32(response.payload + 88) == ESP_OK);
    rt.tx_wire_length = 0; request.header.request_id++;
    inventory_result = ESP_ERR_INVALID_CRC;
    assert(executor_dispatch(&rt, &request, now_us)); executor_run_one();
    assert(executor_queue_next(&rt));
    assert(iris_frame_decode_in_place(rt.tx_wire, rt.tx_wire_length - 1, &response) == ESP_OK);
    assert(response.header.type == ESP_IRIS_CONTROL_ERROR);
    assert(iris_get_le32(response.payload) == ESP_ERR_INVALID_CRC);
    rt.tx_wire_length = 0; request.header.payload_size = 1;
    assert(executor_dispatch(&rt, &request, now_us)); executor_run_one();
    assert(executor_queue_next(&rt));
    assert(iris_frame_decode_in_place(rt.tx_wire, rt.tx_wire_length - 1, &response) == ESP_OK);
    assert(iris_get_le32(response.payload) == ESP_ERR_INVALID_SIZE && inventory_reads == 2);
    assert(esp_iris_system_inventory_unregister(NULL) == ESP_OK);
    responsive_runtime = NULL;
}
#endif
static void test_unsupported_executor_channel_type(void) {
    iris_runtime_t rt = {.session_id = 960};
    uint8_t payload[1] = {0};
    iris_decoded_frame_t request = {.header = {.channel = ESP_IRIS_CHANNEL_SYSTEM_UPDATE,
        .type = 0xff, .request_id = 200}, .payload = payload};
    assert(executor_dispatch(&rt, &request, now_us)); executor_run_one();
    assert(executor_queue_next(&rt));
    iris_decoded_frame_t response;
    assert(iris_frame_decode_in_place(rt.tx_wire, rt.tx_wire_length - 1, &response) == ESP_OK);
    assert(response.header.type == ESP_IRIS_CONTROL_ERROR &&
           iris_get_le32(response.payload) == ESP_ERR_NOT_SUPPORTED);
}
/* Fixed seed, bounded corpus, production COBS parser and response path. */
static uint32_t fuzz_random(uint32_t *seed) {
    *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
    return *seed;
}
static void test_fragmented_malformed_corpus(void) {
    uint32_t seed = UINT32_C(0x49524953);
    iris_runtime_t rt = {.session_id = 800, .hello_acked = true};
    for (unsigned round = 0; round < 512; ++round) {
        size_t remaining = fuzz_random(&seed) % (ESP_IRIS_MAX_WIRE_FRAME_SIZE + 128);
        while (remaining != 0) {
            uint8_t noise[61];
            size_t n = 1 + fuzz_random(&seed) % sizeof(noise);
            if (n > remaining) n = remaining;
            for (size_t i = 0; i < n; ++i) noise[i] = (uint8_t)fuzz_random(&seed);
            size_t offset = 0;
            while (offset < n) {
                offset += feed_rx(&rt, noise + offset, n - offset);
                assert(rt.rx_wire_length < sizeof(rt.rx_wire));
                assert(rt.tx_wire_length <= sizeof(rt.tx_wire));
                rt.tx_wire_length = 0;
            }
            remaining -= n;
        }
        const uint8_t delimiter = 0;
        (void)feed_rx(&rt, &delimiter, 1); rt.tx_wire_length = 0;
        uint8_t wire[64]; size_t length;
        esp_iris_wire_header_t h = {.channel = ESP_IRIS_CHANNEL_CONTROL,
            .type = ESP_IRIS_CONTROL_PING, .session_id = rt.session_id,
            .request_id = 1000 + round, .sequence = round + 1};
        assert(iris_frame_encode(wire, sizeof(wire), &h, NULL, 0, &length) == ESP_OK);
        for (size_t i = 0; i < length; ++i) assert(feed_rx(&rt, wire + i, 1) == 1);
        iris_decoded_frame_t response;
        assert(rt.tx_wire_length > 0);
        assert(iris_frame_decode_in_place(rt.tx_wire, rt.tx_wire_length - 1, &response) == ESP_OK);
        assert(response.header.type == ESP_IRIS_CONTROL_PONG &&
               response.header.request_id == h.request_id);
        rt.tx_wire_length = 0;
    }
}
static void test_executor_idle_reap(void) {
    if (s_executor.work == NULL) {
        s_executor.work = calloc(1, sizeof(*s_executor.work));
        assert(s_executor.work != NULL);
    }
    s_executor.task = (TaskHandle_t)(uintptr_t)1;
    atomic_store(&s_executor.stage, IRIS_EXEC_IDLE);
    assert(!executor_reap_idle((TaskHandle_t)(uintptr_t)2));
    assert(s_executor.work != NULL && s_executor.task != NULL);
    assert(executor_reap_idle((TaskHandle_t)(uintptr_t)1));
    assert(s_executor.work == NULL && s_executor.task == NULL);
}
int main(void) {
    test_rpc_lengths(); test_coalesced_frames(); test_replay_and_reopen();
    test_claim_timeout(); test_executor_rpc(); test_fragmented_malformed_corpus();
#if CONFIG_ESP_IRIS_OTA
    test_executor_ota();
#endif
#if CONFIG_ESP_IRIS_SYSTEM_UPDATE
    test_executor_system_update();
#endif
#if CONFIG_ESP_IRIS_SYSTEM_INVENTORY
    test_inventory_executor_dispatch();
#endif
    test_unsupported_executor_channel_type();
    test_executor_idle_reap();
    free(s_executor.work); return 0;
}
