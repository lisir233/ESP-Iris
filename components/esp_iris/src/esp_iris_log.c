#include "esp_iris_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"

#define IRIS_LOG_VFS_PATH "/dev/iris"
#define IRIS_STDIO_BUFFER_SIZE 256U
#define IRIS_LOG_STORAGE_MAGIC UINT32_C(0x474F4C49) /* "ILOG" */
#define IRIS_LOG_STORAGE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t capacity;
    uint32_t write_offset;
    uint32_t oldest_offset;
    uint32_t retained_bytes;
    uint32_t total_bytes;
    uint8_t data[CONFIG_ESP_IRIS_LOG_RING_BYTES];
} iris_log_storage_t;

/* ESP-IDF only installs the Core Dump linker fragment when Core Dump is
 * enabled. Use ordinary BSS sections in builds that only need live logs. */
#if CONFIG_ESP_COREDUMP_ENABLE && CONFIG_ESP_IRIS_LOG_RING_STORAGE_PSRAM
#define IRIS_LOG_STORAGE_ATTR COREDUMP_EXTRAM_ATTR
#elif CONFIG_ESP_COREDUMP_ENABLE
#define IRIS_LOG_STORAGE_ATTR COREDUMP_DRAM_ATTR
#elif CONFIG_ESP_IRIS_LOG_RING_STORAGE_PSRAM
#define IRIS_LOG_STORAGE_ATTR EXT_RAM_BSS_ATTR
#else
#define IRIS_LOG_STORAGE_ATTR
#endif

IRIS_LOG_STORAGE_ATTR __attribute__((used, aligned(4)))
iris_log_storage_t g_iris_log_storage;

static char s_stdout_buffer[IRIS_STDIO_BUFFER_SIZE];
static char s_stderr_buffer[IRIS_STDIO_BUFFER_SIZE];
static FILE *s_previous_stdout;
static FILE *s_previous_stderr;
static FILE *s_iris_stdout;
static FILE *s_iris_stderr;

static int log_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    (void)flags;
    (void)mode;
    if (strcmp(path, "/stdout") == 0) {
        return 1;
    }
    if (strcmp(path, "/stderr") == 0) {
        return 2;
    }
    errno = ENOENT;
    return -1;
}

static int log_close(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return 0;
}

static int log_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    (void)fd;
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static size_t ring_capacity(void)
{
    return sizeof(g_iris_log_storage.data);
}

static void ring_copy_out(size_t offset, uint8_t *data, size_t length)
{
    const size_t capacity = ring_capacity();
    const size_t first = length < capacity - offset
        ? length : capacity - offset;
    memcpy(data, &g_iris_log_storage.data[offset], first);
    if (length > first) {
        memcpy(data + first, g_iris_log_storage.data, length - first);
    }
}

static void ring_copy_in(const uint8_t *data, size_t length)
{
    const size_t capacity = ring_capacity();
    const size_t offset = g_iris_log_storage.write_offset;
    const size_t first = length < capacity - offset
        ? length : capacity - offset;
    memcpy(&g_iris_log_storage.data[offset], data, first);
    if (length > first) {
        memcpy(g_iris_log_storage.data, data + first, length - first);
    }
    g_iris_log_storage.write_offset = (uint32_t)((offset + length) % capacity);
}

static void ring_read(iris_runtime_t *runtime, uint8_t *data, size_t length)
{
    ring_copy_out(runtime->log_tail, data, length);
    runtime->log_tail = (runtime->log_tail + length) % ring_capacity();
    runtime->log_used -= length;
}

static void ring_peek(const iris_runtime_t *runtime, uint8_t *data,
                      size_t length)
{
    ring_copy_out(runtime->log_tail, data, length);
}

static void drop_oldest(iris_runtime_t *runtime)
{
    uint8_t header[IRIS_LOG_RECORD_HEADER_SIZE];
    if (runtime->log_used < sizeof(header)) {
        runtime->log_tail = g_iris_log_storage.write_offset;
        runtime->log_used = 0;
        return;
    }
    ring_peek(runtime, header, sizeof(header));
    const uint16_t data_length = iris_get_le16(header + 10);
    const size_t record_size = sizeof(header) + data_length;
    if (record_size > runtime->log_used) {
        runtime->log_tail = g_iris_log_storage.write_offset;
        runtime->log_used = 0;
        return;
    }
    runtime->log_tail = (runtime->log_tail + record_size) %
                        ring_capacity();
    runtime->log_used -= record_size;
    runtime->log_dropped_bytes += data_length;
}

static void drop_oldest_history(void)
{
    uint8_t header[IRIS_LOG_RECORD_HEADER_SIZE];
    if (g_iris_log_storage.retained_bytes < sizeof(header)) {
        g_iris_log_storage.oldest_offset =
            g_iris_log_storage.write_offset;
        g_iris_log_storage.retained_bytes = 0;
        return;
    }

    ring_copy_out(g_iris_log_storage.oldest_offset, header, sizeof(header));
    const uint16_t data_length = iris_get_le16(header + 10);
    const size_t record_size = sizeof(header) + data_length;
    if (data_length > IRIS_LOG_RECORD_DATA_MAX ||
            record_size > g_iris_log_storage.retained_bytes) {
        g_iris_log_storage.oldest_offset =
            g_iris_log_storage.write_offset;
        g_iris_log_storage.retained_bytes = 0;
        return;
    }

    g_iris_log_storage.oldest_offset =
        (uint32_t)((g_iris_log_storage.oldest_offset + record_size) %
                   ring_capacity());
    g_iris_log_storage.retained_bytes -= (uint32_t)record_size;
}

static void ring_write_record(iris_runtime_t *runtime,
                              const uint8_t *header,
                              const uint8_t *data, size_t length)
{
    const size_t record_size = IRIS_LOG_RECORD_HEADER_SIZE + length;
    while (ring_capacity() - g_iris_log_storage.retained_bytes < record_size &&
           g_iris_log_storage.retained_bytes > 0) {
        drop_oldest_history();
    }

    ring_copy_in(header, IRIS_LOG_RECORD_HEADER_SIZE);
    ring_copy_in(data, length);
    runtime->log_used += record_size;
    g_iris_log_storage.retained_bytes += (uint32_t)record_size;
    g_iris_log_storage.total_bytes += (uint32_t)record_size;
}

static void ring_reset(iris_runtime_t *runtime)
{
    /* Old payload bytes are ignored while retained_bytes is zero. Avoid an
     * 8 KiB PSRAM clear while holding the cross-core log lock. */
    memset(&g_iris_log_storage, 0,
           offsetof(iris_log_storage_t, data));
    g_iris_log_storage.magic = IRIS_LOG_STORAGE_MAGIC;
    g_iris_log_storage.version = IRIS_LOG_STORAGE_VERSION;
    g_iris_log_storage.header_size =
        (uint16_t)offsetof(iris_log_storage_t, data);
    g_iris_log_storage.capacity = (uint32_t)ring_capacity();
    runtime->log_tail = 0;
    runtime->log_used = 0;
    runtime->log_dropped_bytes = 0;
}

static ssize_t log_write(void *ctx, int fd, const void *data, size_t size)
{
    iris_runtime_t *runtime = ctx;
    if (runtime == NULL || data == NULL) {
        errno = EINVAL;
        return -1;
    }

    const uint8_t source = fd == 2 ? 2U : 1U;
    const uint8_t *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        const size_t chunk = size - offset > IRIS_LOG_RECORD_DATA_MAX
            ? IRIS_LOG_RECORD_DATA_MAX : size - offset;
        uint8_t header[IRIS_LOG_RECORD_HEADER_SIZE];
        iris_put_le64(header, (uint64_t)esp_timer_get_time());
        header[8] = source;
        header[9] = 0;
        iris_put_le16(header + 10, (uint16_t)chunk);

        taskENTER_CRITICAL(&runtime->log_lock);
        const size_t needed = sizeof(header) + chunk;
        while (ring_capacity() - runtime->log_used < needed &&
               runtime->log_used > 0) {
            drop_oldest(runtime);
        }
        if (ring_capacity() - runtime->log_used >= needed) {
            ring_write_record(runtime, header, bytes + offset, chunk);
        } else {
            runtime->log_dropped_bytes += chunk;
        }
        taskEXIT_CRITICAL(&runtime->log_lock);
        offset += chunk;
    }

    if (runtime->task != NULL) {
        xTaskNotifyGive(runtime->task);
    }
    /* stdout must remain nonblocking even when Iris drops a record. */
    return (ssize_t)size;
}

static const esp_vfs_fs_ops_t s_log_vfs = {
    .write_p = log_write,
    .open_p = log_open,
    .close_p = log_close,
    .fstat_p = log_fstat,
};

esp_err_t iris_log_vfs_init(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&runtime->log_lock);
    ring_reset(runtime);
    taskEXIT_CRITICAL(&runtime->log_lock);
    return esp_vfs_register_fs(IRIS_LOG_VFS_PATH, &s_log_vfs,
                               ESP_VFS_FLAG_STATIC |
                               ESP_VFS_FLAG_CONTEXT_PTR,
                               runtime);
}

esp_err_t iris_log_vfs_deinit(void)
{
    return esp_vfs_unregister_fs(IRIS_LOG_VFS_PATH);
}

esp_err_t iris_log_redirect_stdio(void)
{
    if (s_previous_stdout != NULL || s_previous_stderr != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_iris_stdout = fopen(IRIS_LOG_VFS_PATH "/stdout", "w");
    s_iris_stderr = fopen(IRIS_LOG_VFS_PATH "/stderr", "w");
    if (s_iris_stdout == NULL || s_iris_stderr == NULL) {
        if (s_iris_stdout != NULL) {
            fclose(s_iris_stdout);
        }
        if (s_iris_stderr != NULL) {
            fclose(s_iris_stderr);
        }
        s_iris_stdout = NULL;
        s_iris_stderr = NULL;
        return ESP_FAIL;
    }
    s_previous_stdout = stdout;
    s_previous_stderr = stderr;
    stdout = s_iris_stdout;
    stderr = s_iris_stderr;
    if (setvbuf(s_iris_stdout, s_stdout_buffer, _IOLBF,
                sizeof(s_stdout_buffer)) != 0 ||
            setvbuf(s_iris_stderr, s_stderr_buffer, _IOLBF,
                    sizeof(s_stderr_buffer)) != 0) {
        goto fail;
    }
    return ESP_OK;

fail:
    (void)iris_log_restore_stdio();
    return ESP_FAIL;
}

esp_err_t iris_log_restore_stdio(void)
{
    if (s_previous_stdout == NULL || s_previous_stderr == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)fflush(s_iris_stdout);
    (void)fflush(s_iris_stderr);
    stdout = s_previous_stdout;
    stderr = s_previous_stderr;
    const int stdout_result = fclose(s_iris_stdout);
    const int stderr_result = fclose(s_iris_stderr);
    s_previous_stdout = NULL;
    s_previous_stderr = NULL;
    s_iris_stdout = NULL;
    s_iris_stderr = NULL;
    return stdout_result == 0 && stderr_result == 0 ? ESP_OK : ESP_FAIL;
}

bool iris_log_pop(iris_runtime_t *runtime, size_t payload_budget,
                  iris_log_record_t *out_record)
{
    if (runtime == NULL || out_record == NULL) {
        return false;
    }

    bool available = false;
    taskENTER_CRITICAL(&runtime->log_lock);
    uint8_t header[IRIS_LOG_RECORD_HEADER_SIZE];
    if (runtime->log_used >= sizeof(header)) {
        ring_peek(runtime, header, sizeof(header));
        const uint16_t length = iris_get_le16(header + 10);
        const size_t record_size = sizeof(header) + length;
        const size_t wire_payload = 16U + length;
        if (length <= sizeof(out_record->data) &&
                record_size <= runtime->log_used &&
                wire_payload <= payload_budget) {
            ring_read(runtime, header, sizeof(header));
            out_record->monotonic_us = iris_get_le64(header);
            out_record->source = header[8];
            out_record->flags = header[9];
            out_record->length = length;
            out_record->dropped_total = runtime->log_dropped_bytes;
            ring_read(runtime, out_record->data, length);
            available = true;
        }
    }
    taskEXIT_CRITICAL(&runtime->log_lock);
    return available;
}
