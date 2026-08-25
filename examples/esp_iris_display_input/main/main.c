#include "esp_iris.h"

#include <stdio.h>

#define DISPLAY_WIDTH        480U
#define DISPLAY_HEIGHT       480U
#define DISPLAY_STRIDE       (DISPLAY_WIDTH * 2U)
#define DISPLAY_TOTAL_BYTES  (DISPLAY_STRIDE * DISPLAY_HEIGHT)
#define POINTER_SERVICE_ID   0x1001U
#define POINTER_METHOD_ID    1U
#define POINTER_MESSAGE_SIZE 12U

static uint16_t synthetic_pixel(uint16_t x, uint16_t y)
{
    const uint16_t red = (uint16_t)((x * 31U) / (DISPLAY_WIDTH - 1U));
    const uint16_t green = (uint16_t)((y * 63U) / (DISPLAY_HEIGHT - 1U));
    const uint16_t blue = (uint16_t)((x ^ y) & 0x1fU);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static esp_err_t screen_begin(const esp_iris_media_desc_t *requested,
                              esp_iris_media_desc_t *actual,
                              uint32_t *total_size, void *user_ctx)
{
    (void)requested;
    (void)user_ctx;
    *actual = (esp_iris_media_desc_t) {
        .x = 0,
        .y = 0,
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .stride = DISPLAY_STRIDE,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
        .quality = 0,
    };
    *total_size = DISPLAY_TOTAL_BYTES;
    return ESP_OK;
}

static esp_err_t screen_read(uint32_t offset, uint8_t *out, size_t capacity,
                             size_t *out_size, void *user_ctx)
{
    (void)user_ctx;
    if (offset >= DISPLAY_TOTAL_BYTES || capacity == 0 || out == NULL ||
        out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = DISPLAY_TOTAL_BYTES - offset;
    if (size > capacity) {
        size = capacity;
    }
    for (size_t i = 0; i < size; ++i) {
        const uint32_t byte_index = offset + i;
        const uint32_t pixel_index = byte_index / 2U;
        const uint16_t x = (uint16_t)(pixel_index % DISPLAY_WIDTH);
        const uint16_t y = (uint16_t)(pixel_index / DISPLAY_WIDTH);
        const uint16_t pixel = synthetic_pixel(x, y);
        out[i] = (byte_index & 1U) == 0 ? (uint8_t)pixel
                                        : (uint8_t)(pixel >> 8);
    }
    *out_size = size;
    return ESP_OK;
}

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request,
                             uint8_t *response, size_t response_capacity,
                             size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != POINTER_MESSAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (response_capacity < POINTER_MESSAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < POINTER_MESSAGE_SIZE; ++i) {
        response[i] = request->payload[i];
    }
    *response_size = POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

void app_main(void)
{
    const esp_iris_screen_backend_t screen = {
        .begin = screen_begin,
        .read = screen_read,
        .end = NULL,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_iris_screen_register(&screen));
    ESP_ERROR_CHECK(esp_iris_rpc_register(POINTER_SERVICE_ID,
                                          POINTER_METHOD_ID,
                                          pointer_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());
    printf("ESP-Iris display/input example ready: 480x480 RGB565, "
           "pointer RPC=0x1001/1\n");
}
