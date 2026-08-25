#include "esp_iris_codec.h"

#include <string.h>

static const uint8_t s_magic[4] = {'I', 'R', 'I', 'S'};

uint32_t iris_crc32(const uint8_t *data, size_t length, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static void encode_header(uint8_t out[ESP_IRIS_WIRE_HEADER_SIZE],
                          const esp_iris_wire_header_t *header)
{
    memcpy(out, s_magic, sizeof(s_magic));
    out[4] = ESP_IRIS_PROTOCOL_VERSION;
    out[5] = ESP_IRIS_WIRE_HEADER_SIZE;
    out[6] = header->channel;
    out[7] = header->type;
    iris_put_le16(out + 8, header->flags);
    iris_put_le16(out + 10, 0);
    iris_put_le32(out + 12, header->session_id);
    iris_put_le32(out + 16, header->request_id);
    iris_put_le32(out + 20, header->stream_id);
    iris_put_le32(out + 24, header->sequence);
    iris_put_le32(out + 28, header->payload_size);
}

typedef struct {
    uint8_t *out;
    size_t capacity;
    size_t length;
    size_t code_offset;
    uint8_t code;
} cobs_writer_t;

static bool cobs_begin(cobs_writer_t *writer, uint8_t *out, size_t capacity)
{
    if (capacity < 2) {
        return false;
    }
    *writer = (cobs_writer_t) {
        .out = out,
        .capacity = capacity,
        .length = 1,
        .code_offset = 0,
        .code = 1,
    };
    return true;
}

static bool cobs_put(cobs_writer_t *writer, uint8_t value)
{
    if (value == 0) {
        writer->out[writer->code_offset] = writer->code;
        if (writer->length >= writer->capacity) {
            return false;
        }
        writer->code_offset = writer->length++;
        writer->code = 1;
        return true;
    }

    if (writer->length >= writer->capacity) {
        return false;
    }
    writer->out[writer->length++] = value;
    ++writer->code;
    if (writer->code == 0xff) {
        writer->out[writer->code_offset] = writer->code;
        if (writer->length >= writer->capacity) {
            return false;
        }
        writer->code_offset = writer->length++;
        writer->code = 1;
    }
    return true;
}

static bool cobs_write(cobs_writer_t *writer, const uint8_t *data,
                       size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (!cobs_put(writer, data[i])) {
            return false;
        }
    }
    return true;
}

static bool cobs_finish(cobs_writer_t *writer, size_t *out_size)
{
    writer->out[writer->code_offset] = writer->code;
    if (writer->length >= writer->capacity) {
        return false;
    }
    writer->out[writer->length++] = 0;
    *out_size = writer->length;
    return true;
}

esp_err_t iris_frame_encode(uint8_t *out, size_t out_capacity,
                            const esp_iris_wire_header_t *header,
                            const uint8_t *payload, size_t payload_size,
                            size_t *out_size)
{
    if (out == NULL || header == NULL || out_size == NULL ||
            (payload_size > 0 && payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_size > ESP_IRIS_MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->channel >= ESP_IRIS_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw_header[ESP_IRIS_WIRE_HEADER_SIZE];
    esp_iris_wire_header_t normalized = *header;
    normalized.payload_size = payload_size;
    encode_header(raw_header, &normalized);

    uint32_t crc = iris_crc32(raw_header, sizeof(raw_header), 0);
    crc = iris_crc32(payload, payload_size, crc);
    uint8_t raw_crc[4];
    iris_put_le32(raw_crc, crc);

    cobs_writer_t writer;
    if (!cobs_begin(&writer, out, out_capacity) ||
            !cobs_write(&writer, raw_header, sizeof(raw_header)) ||
            !cobs_write(&writer, payload, payload_size) ||
            !cobs_write(&writer, raw_crc, sizeof(raw_crc)) ||
            !cobs_finish(&writer, out_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t cobs_decode_in_place(uint8_t *buffer, size_t encoded_size,
                                      size_t *decoded_size)
{
    size_t read_offset = 0;
    size_t write_offset = 0;
    while (read_offset < encoded_size) {
        const uint8_t code = buffer[read_offset++];
        if (code == 0) {
            return ESP_ERR_INVALID_CRC;
        }
        const size_t copy_size = (size_t)code - 1U;
        if (copy_size > encoded_size - read_offset) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < copy_size; ++i) {
            buffer[write_offset++] = buffer[read_offset++];
        }
        if (code != 0xff && read_offset < encoded_size) {
            buffer[write_offset++] = 0;
        }
    }
    *decoded_size = write_offset;
    return ESP_OK;
}

esp_err_t iris_frame_decode_in_place(uint8_t *wire, size_t wire_size,
                                     iris_decoded_frame_t *out_frame)
{
    if (wire == NULL || out_frame == NULL || wire_size == 0 ||
            wire_size >= ESP_IRIS_MAX_WIRE_FRAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded_size = 0;
    esp_err_t err = cobs_decode_in_place(wire, wire_size, &decoded_size);
    if (err != ESP_OK) {
        return err;
    }
    if (decoded_size < ESP_IRIS_WIRE_HEADER_SIZE + 4U ||
            memcmp(wire, s_magic, sizeof(s_magic)) != 0 ||
            wire[4] != ESP_IRIS_PROTOCOL_VERSION ||
            wire[5] != ESP_IRIS_WIRE_HEADER_SIZE ||
            wire[6] >= ESP_IRIS_CHANNEL_COUNT ||
            iris_get_le16(wire + 10) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t payload_size = iris_get_le32(wire + 28);
    if (payload_size > ESP_IRIS_MAX_PAYLOAD_SIZE ||
            decoded_size != ESP_IRIS_WIRE_HEADER_SIZE + payload_size + 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t expected_crc = iris_get_le32(wire + decoded_size - 4U);
    const uint32_t actual_crc = iris_crc32(wire, decoded_size - 4U, 0);
    if (expected_crc != actual_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    out_frame->header = (esp_iris_wire_header_t) {
        .channel = wire[6],
        .type = wire[7],
        .flags = iris_get_le16(wire + 8),
        .session_id = iris_get_le32(wire + 12),
        .request_id = iris_get_le32(wire + 16),
        .stream_id = iris_get_le32(wire + 20),
        .sequence = iris_get_le32(wire + 24),
        .payload_size = payload_size,
    };
    out_frame->payload = wire + ESP_IRIS_WIRE_HEADER_SIZE;
    return ESP_OK;
}
