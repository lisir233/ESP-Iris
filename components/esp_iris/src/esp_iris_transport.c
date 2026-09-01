#include "esp_iris_internal.h"

#include <errno.h>
#include "esp_timer.h"

#define IRIS_CLAIM_TIMEOUT_US \
    ((int64_t)CONFIG_ESP_IRIS_TRANSPORT_CLAIM_TIMEOUT_MS * 1000LL)

static const iris_transport_ops_t *const s_transports[] = {
#if CONFIG_ESP_IRIS_TRANSPORT_USB
    &g_iris_usb_transport_ops,
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_TCP
    &g_iris_tcp_transport_ops,
#endif
#if CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG
    &g_iris_usb_serial_jtag_transport_ops,
#endif
};

#define IRIS_TRANSPORT_COUNT \
    (sizeof(s_transports) / sizeof(s_transports[0]))

static iris_transport_state_t *state_for(iris_runtime_t *runtime,
                                         esp_iris_transport_kind_t kind)
{
    switch (kind) {
    case ESP_IRIS_TRANSPORT_KIND_USB:
        return &runtime->transport.usb;
    case ESP_IRIS_TRANSPORT_KIND_TCP:
        return &runtime->transport.tcp;
    case ESP_IRIS_TRANSPORT_KIND_USB_SERIAL_JTAG:
        return &runtime->transport.usb_serial_jtag;
    default:
        return NULL;
    }
}

static void clear_active(iris_transport_manager_t *manager)
{
    manager->active_ops = NULL;
    manager->active_state = NULL;
    manager->claim_deadline_us = 0;
    manager->committed = false;
}

static void stop_losers(iris_runtime_t *runtime)
{
    iris_transport_manager_t *manager = &runtime->transport;
    for (size_t i = 0; i < IRIS_TRANSPORT_COUNT; ++i) {
        const iris_transport_ops_t *ops = s_transports[i];
        if (ops == manager->active_ops) {
            continue;
        }
        iris_transport_state_t *state = state_for(runtime, ops->kind);
        if (state != NULL && state->driver_started) {
            ops->stop(runtime, state);
        }
    }
}

static void start_missing(iris_runtime_t *runtime)
{
    for (size_t i = 0; i < IRIS_TRANSPORT_COUNT; ++i) {
        const iris_transport_ops_t *ops = s_transports[i];
        iris_transport_state_t *state = state_for(runtime, ops->kind);
        if (state != NULL && !state->driver_started) {
            (void)ops->start(runtime, state);
        }
    }
}

esp_err_t iris_transport_start(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_transport_manager_t *manager = &runtime->transport;
    clear_active(manager);
    manager->scan_start = 0;
    for (size_t i = 0; i < sizeof(manager->retry_after_us) /
                            sizeof(manager->retry_after_us[0]); ++i) {
        manager->retry_after_us[i] = 0;
    }

    for (size_t i = 0; i < IRIS_TRANSPORT_COUNT; ++i) {
        const iris_transport_ops_t *ops = s_transports[i];
        iris_transport_state_t *state = state_for(runtime, ops->kind);
        esp_err_t err = ops->start(runtime, state);
        if (err != ESP_OK) {
            for (size_t started = 0; started < i; ++started) {
                const iris_transport_ops_t *started_ops =
                    s_transports[started];
                iris_transport_state_t *started_state =
                    state_for(runtime, started_ops->kind);
                started_ops->stop(runtime, started_state);
            }
            return err;
        }
    }
    return ESP_OK;
}

void iris_transport_stop(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    for (size_t i = 0; i < IRIS_TRANSPORT_COUNT; ++i) {
        const iris_transport_ops_t *ops = s_transports[i];
        iris_transport_state_t *state = state_for(runtime, ops->kind);
        if (state != NULL && state->driver_started) {
            ops->stop(runtime, state);
        }
    }
    clear_active(&runtime->transport);
}

iris_link_event_t iris_transport_poll(iris_runtime_t *runtime)
{
    iris_transport_manager_t *manager = &runtime->transport;
    const int64_t now = esp_timer_get_time();

    if (manager->active_ops != NULL) {
        const iris_link_event_t event = manager->active_ops->poll(
            runtime, manager->active_state);
        if (event == IRIS_LINK_EVENT_DISCONNECTED) {
            clear_active(manager);
            start_missing(runtime);
            return IRIS_LINK_EVENT_DISCONNECTED;
        }
        if (!manager->committed && now >= manager->claim_deadline_us) {
            const esp_iris_transport_kind_t rejected_kind =
                manager->active_ops->kind;
            manager->active_ops->stop(runtime, manager->active_state);
            if (rejected_kind == ESP_IRIS_TRANSPORT_KIND_USB_SERIAL_JTAG) {
                manager->retry_after_us[rejected_kind] =
                    now + IRIS_CLAIM_TIMEOUT_US;
            }
            clear_active(manager);
            start_missing(runtime);
            return IRIS_LINK_EVENT_DISCONNECTED;
        }
        return IRIS_LINK_EVENT_NONE;
    }

    start_missing(runtime);
    for (size_t offset = 0; offset < IRIS_TRANSPORT_COUNT; ++offset) {
        const size_t index = (manager->scan_start + offset) %
                             IRIS_TRANSPORT_COUNT;
        const iris_transport_ops_t *ops = s_transports[index];
        if (manager->retry_after_us[ops->kind] > now) {
            continue;
        }
        iris_transport_state_t *state = state_for(runtime, ops->kind);
        if (state == NULL || !state->driver_started) {
            continue;
        }
        if (ops->poll(runtime, state) != IRIS_LINK_EVENT_CONNECTED) {
            continue;
        }
        manager->active_ops = ops;
        manager->active_state = state;
        manager->scan_start = (uint8_t)((index + 1U) %
                                        IRIS_TRANSPORT_COUNT);
        manager->committed = IRIS_TRANSPORT_COUNT == 1U;
        manager->claim_deadline_us = now + IRIS_CLAIM_TIMEOUT_US;
        if (manager->committed) {
            stop_losers(runtime);
        }
        return IRIS_LINK_EVENT_CONNECTED;
    }
    return IRIS_LINK_EVENT_NONE;
}

int iris_transport_read(iris_runtime_t *runtime, uint8_t *buffer,
                        size_t capacity)
{
    iris_transport_manager_t *manager = &runtime->transport;
    if (manager->active_ops == NULL) {
        return -ENOTCONN;
    }
    return manager->active_ops->read(runtime, manager->active_state,
                                     buffer, capacity);
}

int iris_transport_write(iris_runtime_t *runtime, const uint8_t *buffer,
                         size_t length)
{
    iris_transport_manager_t *manager = &runtime->transport;
    if (manager->active_ops == NULL) {
        return -ENOTCONN;
    }
    return manager->active_ops->write(runtime, manager->active_state,
                                      buffer, length);
}

esp_iris_transport_kind_t iris_transport_kind(void)
{
    const iris_transport_manager_t *manager = &g_iris.transport;
    if (manager->active_ops != NULL) {
        return manager->active_ops->kind;
    }
    return IRIS_TRANSPORT_COUNT == 1U ? s_transports[0]->kind
                                     : ESP_IRIS_TRANSPORT_KIND_NONE;
}

const char *iris_transport_name(void)
{
    const iris_transport_manager_t *manager = &g_iris.transport;
    if (manager->active_ops != NULL) {
        return manager->active_ops->name;
    }
    return IRIS_TRANSPORT_COUNT == 1U ? s_transports[0]->name : "waiting";
}

void iris_transport_commit(iris_runtime_t *runtime)
{
    if (runtime == NULL || runtime->transport.active_ops == NULL ||
            runtime->transport.committed) {
        return;
    }
    runtime->transport.committed = true;
    runtime->transport.claim_deadline_us = 0;
    stop_losers(runtime);
}

void iris_transport_disconnect(iris_runtime_t *runtime)
{
    if (runtime == NULL || runtime->transport.active_ops == NULL ||
            runtime->transport.active_ops->disconnect == NULL) {
        return;
    }
    runtime->transport.active_ops->disconnect(
        runtime, runtime->transport.active_state);
}
