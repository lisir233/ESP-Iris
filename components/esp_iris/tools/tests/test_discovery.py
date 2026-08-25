from types import SimpleNamespace

from serial.tools import list_ports

from iris_gateway.discovery import discover_iris_usb_devices


def test_usb_discovery_filters_iris_interfaces(monkeypatch) -> None:
    monkeypatch.setattr(
        list_ports,
        "comports",
        lambda: [
            SimpleNamespace(
                device="/dev/ttyACM1",
                vid=0x303A,
                pid=0x4002,
                product="ESP-Iris",
                serial_number="iris-1",
            ),
            SimpleNamespace(
                device="/dev/ttyACM0",
                vid=0x303A,
                pid=0x1001,
                product="USB JTAG/serial debug unit",
                serial_number="debug-1",
            ),
        ],
    )
    devices = discover_iris_usb_devices()
    assert [(item.device, item.pid) for item in devices] == [
        ("/dev/ttyACM1", 0x4002)
    ]


def test_usb_serial_jtag_discovery_requires_explicit_opt_in(monkeypatch) -> None:
    monkeypatch.setattr(
        list_ports,
        "comports",
        lambda: [
            SimpleNamespace(
                device="/dev/ttyACM0",
                vid=0x303A,
                pid=0x1001,
                product="USB JTAG/serial debug unit",
                serial_number="debug-1",
            )
        ],
    )
    assert discover_iris_usb_devices() == []
    devices = discover_iris_usb_devices(include_usb_serial_jtag=True)
    assert [(item.device, item.transport) for item in devices] == [
        ("/dev/ttyACM0", "usb_serial_jtag")
    ]
