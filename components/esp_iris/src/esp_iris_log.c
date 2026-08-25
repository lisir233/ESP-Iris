#include "esp_iris_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"

#define IRIS_LOG_VFS_PATH "/dev/iris"
#define IRIS_STDIO_BUFFER_SIZE 256U

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

static void ring_write(iris_runtime_t *runtime, const uint8_t *data,
                       size_t length)
{
    const size_t capacity = sizeof(runtime->log_ring);
    for (size_t i = 0; i < length; ++i) {
        runtime->log_ring[runtime->log_head] = data[i];
        runtime->log_head = (runtime->log_head + 1U) % capacity;
    }
    runtime->log_used += length;
}

static void ring_read(iris_runtime_t *runtime, uint8_t *data, size_t length)
{
    const size_t capacity = sizeof(runtime->log_ring);
    for (size_t i = 0; i < length; ++i) {
        data[i] = runtime->log_ring[runtime->log_tail];
        runtime->log_tail = (runtime->log_tail + 1U) % capacity;
    }
    runtime->log_used -= length;
}

static void ring_peek(const iris_runtime_t *runtime, uint8_t *data,
                      size_t length)
{
    const size_t capacity = sizeof(runtime->log_ring);
    size_t offset = runtime->log_tail;
    for (size_t i = 0; i < length; ++i) {
        data[i] = runtime->log_ring[offset];
        offset = (offset + 1U) % capacity;
    }
}

static void drop_oldest(iris_runtime_t *runtime)
{
    uint8_t header[IRIS_LOG_RECORD_HEADER_SIZE];
    if (runtime->log_used < sizeof(header)) {
        runtime->log_tail = runtime->log_head;
        runtime->log_used = 0;
        return;
    }
    ring_peek(runtime, header, sizeof(header));
    const uint16_t data_length = iris_get_le16(header + 10);
    const size_t record_size = sizeof(header) + data_length;
    if (record_size > runtime->log_used) {
        runtime->log_tail = runtime->log_head;
        runtime->log_used = 0;
        return;
    }
    runtime->log_tail = (runtime->log_tail + record_size) %
                        sizeof(runtime->log_ring);
    runtime->log_used -= record_size;
    runtime->log_dropped_bytes += data_length;
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
        while (sizeof(runtime->log_ring) - runtime->log_used < needed &&
               runtime->log_used > 0) {
            drop_oldest(runtime);
        }
        if (sizeof(runtime->log_ring) - runtime->log_used >= needed) {
            ring_write(runtime, header, sizeof(header));
            ring_write(runtime, bytes + offset, chunk);
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
