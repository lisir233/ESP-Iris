#include "esp_iris_internal.h"

#include <limits.h>
#include <string.h>

#include "esp_system.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
} crash_tlv_writer_t;

static bool crash_tlv_put(crash_tlv_writer_t *writer, uint8_t tag,
                          const void *value, size_t length)
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

static bool crash_tlv_put_u8(crash_tlv_writer_t *writer, uint8_t tag,
                             uint8_t value)
{
    return crash_tlv_put(writer, tag, &value, sizeof(value));
}

static bool crash_tlv_put_u32(crash_tlv_writer_t *writer, uint8_t tag,
                              uint32_t value)
{
    uint8_t bytes[4];
    iris_put_le32(bytes, value);
    return crash_tlv_put(writer, tag, bytes, sizeof(bytes));
}

static bool crash_tlv_put_u64(crash_tlv_writer_t *writer, uint8_t tag,
                              uint64_t value)
{
    uint8_t bytes[8];
    iris_put_le64(bytes, value);
    return crash_tlv_put(writer, tag, bytes, sizeof(bytes));
}

static bool reset_is_crash(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

void iris_crash_probe(iris_runtime_t *runtime)
{
    if (runtime == NULL || runtime->crash_initialized) {
        return;
    }
    runtime->previous_boot_crash = reset_is_crash(esp_reset_reason());
    runtime->core_dump_partition = NULL;
    runtime->core_dump_address = 0;
    runtime->core_dump_size = 0;
    runtime->core_dump_present = false;
    runtime->core_dump_valid = false;
    runtime->core_dump_checked = false;
    runtime->core_dump_elf_sha256[0] = '\0';
    runtime->core_dump_elf_sha256_length = 0;
    runtime->panic_reason[0] = '\0';

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t address = 0;
    size_t size = 0;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (partition != NULL && esp_core_dump_image_get(&address, &size) == ESP_OK &&
            address >= partition->address &&
            address - partition->address <= partition->size &&
            size <= partition->size - (address - partition->address)) {
        runtime->core_dump_partition = partition;
        runtime->core_dump_address = address;
        runtime->core_dump_size = size;
        runtime->core_dump_present = true;
    }
#endif
    runtime->crash_initialized = true;
}

static void crash_refresh(iris_runtime_t *runtime)
{
    if (runtime == NULL || runtime->core_dump_checked ||
            !runtime->core_dump_present) {
        return;
    }
    runtime->core_dump_checked = true;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    runtime->core_dump_valid = true;

    esp_core_dump_summary_t summary = {0};
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
        const size_t length = strnlen((const char *)summary.app_elf_sha256,
                                     sizeof(summary.app_elf_sha256));
        const size_t copied = length < sizeof(runtime->core_dump_elf_sha256) - 1
            ? length : sizeof(runtime->core_dump_elf_sha256) - 1;
        memcpy(runtime->core_dump_elf_sha256, summary.app_elf_sha256, copied);
        runtime->core_dump_elf_sha256[copied] = '\0';
        runtime->core_dump_elf_sha256_length = (uint8_t)copied;
    }
    (void)esp_core_dump_get_panic_reason(runtime->panic_reason,
                                         sizeof(runtime->panic_reason));
#endif
}

esp_err_t iris_crash_build_metadata(iris_runtime_t *runtime, uint8_t *out,
                                    size_t capacity, size_t *out_size)
{
    if (runtime == NULL || out == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    crash_refresh(runtime);
    crash_tlv_writer_t writer = {.data = out, .capacity = capacity};
    const uint32_t dump_size = runtime->core_dump_size <= UINT32_MAX
        ? (uint32_t)runtime->core_dump_size : UINT32_MAX;
    if (!crash_tlv_put(&writer, ESP_IRIS_TLV_DEVICE_ID, runtime->device_id,
                       sizeof(runtime->device_id)) ||
            !crash_tlv_put_u64(&writer, ESP_IRIS_TLV_BOOT_ID,
                               runtime->boot_id) ||
            !crash_tlv_put_u32(&writer, ESP_IRIS_TLV_RESET_REASON,
                               (uint32_t)esp_reset_reason()) ||
            !crash_tlv_put_u8(&writer, ESP_IRIS_TLV_PREVIOUS_BOOT_CRASH,
                              runtime->previous_boot_crash ? 1U : 0U) ||
            !crash_tlv_put_u8(&writer, ESP_IRIS_TLV_CORE_DUMP_PRESENT,
                              runtime->core_dump_present ? 1U : 0U) ||
            !crash_tlv_put_u8(&writer, ESP_IRIS_TLV_CORE_DUMP_VALID,
                              runtime->core_dump_valid ? 1U : 0U) ||
            !crash_tlv_put_u32(&writer, ESP_IRIS_TLV_CORE_DUMP_SIZE,
                               dump_size) ||
            !crash_tlv_put(&writer, ESP_IRIS_TLV_CORE_DUMP_ELF_SHA256,
                           runtime->core_dump_elf_sha256,
                           runtime->core_dump_elf_sha256_length) ||
            !crash_tlv_put_u8(
                &writer, ESP_IRIS_TLV_CORE_DUMP_ELF_SHA256_COMPLETE,
                runtime->core_dump_elf_sha256_length == 64U ? 1U : 0U) ||
            !crash_tlv_put_u32(&writer,
                               ESP_IRIS_TLV_CORE_DUMP_CHUNK_MAX,
                               CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES) ||
            !crash_tlv_put(&writer, ESP_IRIS_TLV_PANIC_REASON,
                           runtime->panic_reason,
                           strnlen(runtime->panic_reason,
                                   sizeof(runtime->panic_reason)))) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_size = writer.length;
    return ESP_OK;
}

esp_err_t iris_crash_read(iris_runtime_t *runtime, size_t offset,
                          size_t maximum, uint8_t *out, size_t *out_size)
{
    if (runtime == NULL || out == NULL || out_size == NULL || maximum == 0 ||
            maximum > CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    crash_refresh(runtime);
    if (!runtime->core_dump_valid || runtime->core_dump_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset > runtime->core_dump_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t remaining = runtime->core_dump_size - offset;
    const size_t length = remaining < maximum ? remaining : maximum;
    if (length == 0) {
        *out_size = 0;
        return ESP_OK;
    }
    const size_t partition_offset = runtime->core_dump_address -
                                    runtime->core_dump_partition->address;
    esp_err_t err = esp_partition_read(runtime->core_dump_partition,
                                       partition_offset + offset,
                                       out, length);
    if (err == ESP_OK) {
        *out_size = length;
    }
    return err;
}
