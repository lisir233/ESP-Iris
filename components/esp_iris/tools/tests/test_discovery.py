from types import SimpleNamespace

from iris_gateway.discovery import discover_iris_usb_devices
from serial.tools import list_ports


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
