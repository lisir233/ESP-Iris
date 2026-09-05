#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/esp_iris.c"
#include "../../../src/esp_iris_services.c"
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
static esp_err_t callback(const esp_iris_rpc_request_t *r, uint8_t *out,
    size_t cap, size_t *size, void *ctx) {
    (void)r; (void)ctx; ++callback_calls; memset(out, 0xa5, cap);
    *size = reported_size; return callback_result;
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
        rt.tx_wire_length = 0; handle_frame(&rt, &frame, 0);
    }
    assert(callback_calls == 3);
    frame.header.request_id = 13; frame.header.sequence = 7;
    rt.tx_wire_length = 0; handle_frame(&rt, &frame, 0);
    assert(callback_calls == 3 && rt.invalid_frames == 1);
    frame.header.sequence = 6; handle_frame(&rt, &frame, 0);
    assert(callback_calls == 3 && rt.invalid_frames == 2);
    /* Same ID with changed body/method cannot invoke a different handler. */
    frame.header.sequence = 8; frame.header.request_id = 12; payload[2] = 2;
    handle_frame(&rt, &frame, 0); assert(callback_calls == 3); payload[2] = 1;
    /* Wrap acceptance and half-space rejection. */
    state.last_rpc_request_id = UINT32_MAX; rt.rx_sequence[0] = UINT32_MAX;
    frame.header.request_id = 1; frame.header.sequence = 0;
    rt.tx_wire_length = 0; handle_frame(&rt, &frame, 0); assert(callback_calls == 4);
    frame.header.request_id = UINT32_C(0x80000001); frame.header.sequence = 1;
    rt.tx_wire_length = 0; handle_frame(&rt, &frame, 0); assert(callback_calls == 4);
    /* A negotiated reopen produces a fresh session; delayed ACK of the old
     * session cannot reset it again, and normal ACK replay remains harmless. */
    frame.header.type = ESP_IRIS_CONTROL_HELLO_ACK;
    frame.header.flags = ESP_IRIS_FLAG_NEW_SESSION; frame.header.payload_size = 0;
    rt.tx_wire_length = 0; handle_frame(&rt, &frame, 0);
    const uint32_t new_session = rt.session_id;
    assert(new_session != 123 && new_session != 0 && !rt.hello_acked);
    assert(!state.rpc_request_seen && !rt.rx_sequence_seen[0]);
    rt.tx_wire_length = 0; handle_frame(&rt, &frame, 0); assert(rt.session_id == new_session);
    frame.header.session_id = new_session; frame.header.flags = 0;
    handle_frame(&rt, &frame, 0); assert(rt.hello_acked);
    handle_frame(&rt, &frame, 0); assert(rt.session_id == new_session && rt.hello_acked);
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
int main(void) { test_rpc_lengths(); test_coalesced_frames(); test_replay_and_reopen(); test_claim_timeout(); return 0; }
