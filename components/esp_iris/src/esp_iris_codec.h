#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_iris_protocol.h"

typedef struct {
    esp_iris_wire_header_t header;
    uint8_t *payload;
} iris_decoded_frame_t;

uint32_t iris_crc32(const uint8_t *data, size_t length, uint32_t crc);
esp_err_t iris_frame_encode(uint8_t *out, size_t out_capacity,
                            const esp_iris_wire_header_t *header,
                            const uint8_t *payload, size_t payload_size,
                            size_t *out_size);
esp_err_t iris_frame_decode_in_place(uint8_t *wire, size_t wire_size,
                                     iris_decoded_frame_t *out_frame);

static inline uint16_t iris_get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t iris_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t iris_get_le64(const uint8_t *p)
{
    return (uint64_t)iris_get_le32(p) |
           ((uint64_t)iris_get_le32(p + 4) << 32);
}

static inline void iris_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void iris_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline void iris_put_le64(uint8_t *p, uint64_t value)
{
    iris_put_le32(p, (uint32_t)value);
    iris_put_le32(p + 4, (uint32_t)(value >> 32));
}
