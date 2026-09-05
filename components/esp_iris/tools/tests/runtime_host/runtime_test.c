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
int main(void) { test_rpc_lengths(); return 0; }
