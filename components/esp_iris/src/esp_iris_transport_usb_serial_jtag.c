#include "esp_iris_internal.h"

#include <errno.h>

#include "driver/usb_serial_jtag.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

#define IRIS_USB_SERIAL_JTAG_WRITE_CHUNK 256U

typedef struct {
    esp_err_t result;
} iris_usb_serial_jtag_uninstall_result_t;

static void uninstall_driver(void *argument)
{
    iris_usb_serial_jtag_uninstall_result_t *result = argument;
    result->result = usb_serial_jtag_driver_uninstall();
}

static esp_err_t usb_serial_jtag_start(iris_runtime_t *runtime,
                                       iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->driver_started) {
        return ESP_OK;
    }
    if (usb_serial_jtag_is_driver_installed()) {
        return ESP_ERR_INVALID_STATE;
    }

    state->usb_serial_jtag_auto_reset_was_disabled =
        REG_GET_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                    USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS) != 0;
#if CONFIG_ESP_IRIS_USB_SERIAL_JTAG_DISABLE_AUTO_RESET
    REG_SET_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
#endif

    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = CONFIG_ESP_IRIS_USB_SERIAL_JTAG_TX_BUFFER_BYTES,
        .rx_buffer_size = CONFIG_ESP_IRIS_USB_SERIAL_JTAG_RX_BUFFER_BYTES,
        .intr_priority = 0,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
#if CONFIG_ESP_IRIS_USB_SERIAL_JTAG_DISABLE_AUTO_RESET
        if (!state->usb_serial_jtag_auto_reset_was_disabled) {
            REG_CLR_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                        USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
        }
#endif
        return err;
    }

    state->usb_serial_jtag_install_core = xPortGetCoreID();
    state->driver_started = true;
    state->link_up = false;
    state->reported_link_up = false;
    return ESP_OK;
}

static void usb_serial_jtag_stop(iris_runtime_t *runtime,
                                 iris_transport_state_t *state)
{
    if (runtime == NULL || state == NULL || !state->driver_started) {
        return;
    }

    iris_usb_serial_jtag_uninstall_result_t result = {
        .result = ESP_FAIL,
    };
    if (xPortGetCoreID() == state->usb_serial_jtag_install_core) {
        uninstall_driver(&result);
    } else {
#if CONFIG_ESP_IPC_ENABLE
        esp_err_t ipc_err = esp_ipc_call_blocking(
            state->usb_serial_jtag_install_core,
            uninstall_driver, &result);
        if (ipc_err != ESP_OK) {
            result.result = ipc_err;
        }
#endif
    }
    if (result.result == ESP_OK) {
        state->driver_started = false;
#if CONFIG_ESP_IRIS_USB_SERIAL_JTAG_DISABLE_AUTO_RESET
        if (!state->usb_serial_jtag_auto_reset_was_disabled) {
            REG_CLR_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                        USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
        }
#endif
    }
    state->link_up = false;
    state->reported_link_up = false;
}

static iris_link_event_t usb_serial_jtag_poll(
    iris_runtime_t *runtime, iris_transport_state_t *state)
{
    (void)runtime;
    state->link_up = state->driver_started && usb_serial_jtag_is_connected();
    if (state->link_up == state->reported_link_up) {
        return IRIS_LINK_EVENT_NONE;
    }
    state->reported_link_up = state->link_up;
    return state->link_up ? IRIS_LINK_EVENT_CONNECTED
                          : IRIS_LINK_EVENT_DISCONNECTED;
}

static int usb_serial_jtag_read(iris_runtime_t *runtime,
                                iris_transport_state_t *state,
                                uint8_t *buffer, size_t capacity)
{
    (void)runtime;
    if (!state->link_up) {
        return -ENOTCONN;
    }
    return usb_serial_jtag_read_bytes(buffer, capacity, 0);
}

static int usb_serial_jtag_write(iris_runtime_t *runtime,
                                 iris_transport_state_t *state,
                                 const uint8_t *buffer, size_t length)
{
    (void)runtime;
    if (!state->link_up) {
        return -ENOTCONN;
    }
    const size_t chunk = length < IRIS_USB_SERIAL_JTAG_WRITE_CHUNK
        ? length : IRIS_USB_SERIAL_JTAG_WRITE_CHUNK;
    return usb_serial_jtag_write_bytes(buffer, chunk, 0);
}

const iris_transport_ops_t g_iris_usb_serial_jtag_transport_ops = {
    .kind = ESP_IRIS_TRANSPORT_KIND_USB_SERIAL_JTAG,
    .name = "usb-serial-jtag",
    .start = usb_serial_jtag_start,
    .stop = usb_serial_jtag_stop,
    .poll = usb_serial_jtag_poll,
    .read = usb_serial_jtag_read,
    .write = usb_serial_jtag_write,
};
