#include "esp_iris.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IMAGE_WIDTH 40U
#define IMAGE_HEIGHT 24U
#define IMAGE_BYTES (IMAGE_WIDTH * IMAGE_HEIGHT * 2U)
#define AUDIO_SAMPLE_RATE 16000U
#define AUDIO_CHANNELS 1U
#define AUDIO_SAMPLES_PER_CHUNK 1600U

/* One period of a signed 16-bit sine wave. Scaling it down leaves headroom. */
static const int16_t s_sine[32] = {
    0, 3196, 6269, 9102, 11585, 13623, 15136, 16069,
    16384, 16069, 15136, 13623, 11585, 9102, 6269, 3196,
    0, -3196, -6269, -9102, -11585, -13623, -15136, -16069,
    -16384, -16069, -15136, -13623, -11585, -9102, -6269, -3196,
};

static uint8_t s_image[IMAGE_BYTES];
static uint8_t s_audio[AUDIO_SAMPLES_PER_CHUNK * sizeof(int16_t)];

static void render_rgb565(uint32_t frame_id)
{
    for (uint32_t y = 0; y < IMAGE_HEIGHT; ++y) {
        for (uint32_t x = 0; x < IMAGE_WIDTH; ++x) {
            const uint16_t red = (uint16_t)((x + frame_id) & 0x1fU);
            const uint16_t green = (uint16_t)(((y * 2U) + frame_id) & 0x3fU);
            const uint16_t blue = (uint16_t)(((x + y) + frame_id) & 0x1fU);
            const uint16_t pixel = (uint16_t)((red << 11) | (green << 5) |
                                               blue);
            const size_t offset = (y * IMAGE_WIDTH + x) * 2U;
            s_image[offset] = (uint8_t)(pixel & 0xffU);
            s_image[offset + 1U] = (uint8_t)(pixel >> 8);
        }
    }
}

static void render_pcm_s16le(uint32_t *phase)
{
    for (size_t i = 0; i < AUDIO_SAMPLES_PER_CHUNK; ++i) {
        const int16_t sample = s_sine[*phase & 31U];
        s_audio[i * 2U] = (uint8_t)((uint16_t)sample & 0xffU);
        s_audio[i * 2U + 1U] = (uint8_t)((uint16_t)sample >> 8);
        /* 16 kHz / 32 samples / step 1 = a 500 Hz test tone. */
        *phase = (*phase + 1U) & 31U;
    }
}

static void media_task(void *arg)
{
    (void)arg;
    const esp_iris_media_desc_t image_description = {
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .stride = IMAGE_WIDTH * 2U,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    };
    /* For AUDIO, width carries sample rate, height carries channel count and
     * stride carries bytes per interleaved sample frame. */
    const esp_iris_media_desc_t audio_description = {
        .width = AUDIO_SAMPLE_RATE,
        .height = AUDIO_CHANNELS,
        .stride = AUDIO_CHANNELS * sizeof(int16_t),
        .format = ESP_IRIS_AUDIO_FORMAT_PCM_S16LE,
    };
    uint32_t image_frame_id = 0;
    uint32_t audio_frame_id = 0;
    uint32_t audio_phase = 0;

    while (true) {
        /* Generating and submitting data only while the host has opened the
         * channel avoids wasting CPU and allocating the Iris media slot. */
        if (esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_IMAGE)) {
            render_rgb565(image_frame_id);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_media_submit(
                ESP_IRIS_CHANNEL_IMAGE, &image_description, ++image_frame_id,
                0, s_image, sizeof(s_image)));
        }
        if (esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_AUDIO)) {
            render_pcm_s16le(&audio_phase);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_media_submit(
                ESP_IRIS_CHANNEL_AUDIO, &audio_description, ++audio_frame_id,
                0, s_audio, sizeof(s_audio)));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());
    ESP_ERROR_CHECK(xTaskCreate(media_task, "iris_media", 3072, NULL, 4,
                                NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    printf("ESP-Iris media streams example ready\n");
}
