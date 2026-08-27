#include "esp_iris_internal.h"

#include <errno.h>
#include <stdio.h>

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static iris_runtime_t *s_runtime;
static iris_transport_state_t *s_state;

static const char s_langid[] = {0x09, 0x04};
static const char *s_string_descriptors[] = {
    s_langid,
    CONFIG_ESP_IRIS_USB_MANUFACTURER,
    CONFIG_ESP_IRIS_USB_PRODUCT,
    NULL,
    "ESP-Iris protocol",
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = CONFIG_ESP_IRIS_USB_VID,
    .idProduct = CONFIG_ESP_IRIS_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    (void)event;
    if (s_runtime != NULL && s_runtime->task != NULL) {
        xTaskNotifyGive(s_runtime->task);
    }
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    if (s_runtime == NULL) {
        return;
    }
    const bool host_open = event->line_state_changed_data.dtr;
    if (!host_open) {
        atomic_store(&s_state->disconnect_pending, true);
    }
    atomic_store(&s_state->host_open, host_open);
    if (s_runtime->task != NULL) {
        xTaskNotifyGive(s_runtime->task);
    }
}

static void usb_device_event_callback(tinyusb_event_t *event, void *arg)
{
    iris_runtime_t *runtime = arg;
    if (runtime == NULL || s_state == NULL || event == NULL) {
        return;
    }

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        break;
    case TINYUSB_EVENT_DETACHED:
        atomic_store(&s_state->host_open, false);
        atomic_store(&s_state->disconnect_pending, true);
        break;
    default:
        return;
    }

    if (runtime->task != NULL) {
        xTaskNotifyGive(runtime->task);
    }
}

static esp_err_t usb_start(iris_runtime_t *runtime,
                           iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->driver_started) {
        return ESP_OK;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(runtime->device_id); ++i) {
        state->usb_serial[i * 2] = hex[runtime->device_id[i] >> 4];
        state->usb_serial[i * 2 + 1] = hex[runtime->device_id[i] & 0x0f];
    }
    state->usb_serial[32] = '\0';
    s_string_descriptors[3] = state->usb_serial;

    s_runtime = runtime;
    s_state = state;

    tinyusb_config_t config =
        TINYUSB_DEFAULT_CONFIG(usb_device_event_callback, runtime);
    config.descriptor.device = &s_device_descriptor;
    config.descriptor.string = s_string_descriptors;
    config.descriptor.string_count = sizeof(s_string_descriptors) /
                                     sizeof(s_string_descriptors[0]);
    esp_err_t err = tinyusb_driver_install(&config);
    if (err != ESP_OK) {
        s_runtime = NULL;
        s_state = NULL;
        return err;
    }

    const tinyusb_config_cdcacm_t cdc = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
        /* DTR only marks a PC session. It never triggers reset/download. */
        .callback_line_state_changed = cdc_line_state_callback,
    };
    err = tinyusb_cdcacm_init(&cdc);
    if (err != ESP_OK) {
        (void)tinyusb_driver_uninstall();
        s_runtime = NULL;
        s_state = NULL;
        return err;
    }

    atomic_store(&state->host_open, false);
    atomic_store(&state->disconnect_pending, false);
    state->driver_started = true;
    state->link_up = false;
    state->reported_link_up = false;
    return ESP_OK;
}

static void usb_stop(iris_runtime_t *runtime, iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL || !state->driver_started) {
        return;
    }
    s_runtime = NULL;
    s_state = NULL;
    (void)tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    (void)tinyusb_driver_uninstall();
    state->driver_started = false;
    atomic_store(&state->host_open, false);
    atomic_store(&state->disconnect_pending, false);
    state->link_up = false;
    state->reported_link_up = false;
}

static iris_link_event_t usb_poll(iris_runtime_t *runtime,
                                  iris_transport_state_t *state)
{
    (void)runtime;
    if (atomic_exchange(&state->disconnect_pending, false) &&
            state->reported_link_up) {
        state->link_up = false;
        state->reported_link_up = false;
        return IRIS_LINK_EVENT_DISCONNECTED;
    }
    state->link_up = state->driver_started && atomic_load(&state->host_open) &&
                     tud_mounted();
    if (state->link_up == state->reported_link_up) {
        return IRIS_LINK_EVENT_NONE;
    }
    state->reported_link_up = state->link_up;
    return state->link_up ? IRIS_LINK_EVENT_CONNECTED
                          : IRIS_LINK_EVENT_DISCONNECTED;
}

static int usb_read(iris_runtime_t *runtime, iris_transport_state_t *state,
                    uint8_t *buffer, size_t capacity)
{
    (void)runtime;
    if (!state->link_up) {
        return -ENOTCONN;
    }
    size_t received = 0;
    esp_err_t err = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buffer, capacity,
                                        &received);
    return err == ESP_OK ? (int)received : -EIO;
}

static int usb_write(iris_runtime_t *runtime, iris_transport_state_t *state,
                     const uint8_t *buffer, size_t length)
{
    (void)runtime;
    if (!state->link_up) {
        return -ENOTCONN;
    }
    const size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                                      buffer, length);
    if (queued > 0) {
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    return (int)queued;
}

const iris_transport_ops_t g_iris_usb_transport_ops = {
    .kind = ESP_IRIS_TRANSPORT_KIND_USB,
    .name = "usb",
    .start = usb_start,
    .stop = usb_stop,
    .poll = usb_poll,
    .read = usb_read,
    .write = usb_write,
};
