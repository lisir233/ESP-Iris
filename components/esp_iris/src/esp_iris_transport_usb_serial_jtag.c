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

esp_err_t iris_transport_start(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->transport.driver_started) {
        return ESP_OK;
    }
    if (usb_serial_jtag_is_driver_installed()) {
        return ESP_ERR_INVALID_STATE;
    }

    runtime->transport.usb_serial_jtag_auto_reset_was_disabled =
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
        if (!runtime->transport.usb_serial_jtag_auto_reset_was_disabled) {
            REG_CLR_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                        USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
        }
#endif
        return err;
    }

    runtime->transport.usb_serial_jtag_install_core = xPortGetCoreID();
    runtime->transport.driver_started = true;
    runtime->transport.link_up = false;
    runtime->transport.reported_link_up = false;
    return ESP_OK;
}

void iris_transport_stop(iris_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->transport.driver_started) {
        return;
    }

    iris_usb_serial_jtag_uninstall_result_t result = {
        .result = ESP_FAIL,
    };
    if (xPortGetCoreID() == runtime->transport.usb_serial_jtag_install_core) {
        uninstall_driver(&result);
    } else {
#if CONFIG_ESP_IPC_ENABLE
        esp_err_t ipc_err = esp_ipc_call_blocking(
            runtime->transport.usb_serial_jtag_install_core,
            uninstall_driver, &result);
        if (ipc_err != ESP_OK) {
            result.result = ipc_err;
        }
#endif
    }
    if (result.result == ESP_OK) {
        runtime->transport.driver_started = false;
#if CONFIG_ESP_IRIS_USB_SERIAL_JTAG_DISABLE_AUTO_RESET
        if (!runtime->transport.usb_serial_jtag_auto_reset_was_disabled) {
            REG_CLR_BIT(USB_SERIAL_JTAG_CHIP_RST_REG,
                        USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS);
        }
#endif
    }
    runtime->transport.link_up = false;
    runtime->transport.reported_link_up = false;
}

iris_link_event_t iris_transport_poll(iris_runtime_t *runtime)
{
    runtime->transport.link_up = runtime->transport.driver_started &&
                                 usb_serial_jtag_is_connected();
    if (runtime->transport.link_up == runtime->transport.reported_link_up) {
        return IRIS_LINK_EVENT_NONE;
    }
    runtime->transport.reported_link_up = runtime->transport.link_up;
    return runtime->transport.link_up ? IRIS_LINK_EVENT_CONNECTED
                                      : IRIS_LINK_EVENT_DISCONNECTED;
}

int iris_transport_read(iris_runtime_t *runtime, uint8_t *buffer,
                        size_t capacity)
{
    if (!runtime->transport.link_up) {
        return -ENOTCONN;
    }
    return usb_serial_jtag_read_bytes(buffer, capacity, 0);
}

int iris_transport_write(iris_runtime_t *runtime, const uint8_t *buffer,
                         size_t length)
{
    if (!runtime->transport.link_up) {
        return -ENOTCONN;
    }
    const size_t chunk = length < IRIS_USB_SERIAL_JTAG_WRITE_CHUNK
        ? length : IRIS_USB_SERIAL_JTAG_WRITE_CHUNK;
    return usb_serial_jtag_write_bytes(buffer, chunk, 0);
}

esp_iris_transport_kind_t iris_transport_kind(void)
{
    return ESP_IRIS_TRANSPORT_KIND_USB_SERIAL_JTAG;
}

const char *iris_transport_name(void)
{
    return "usb-serial-jtag";
}
