from __future__ import annotations

from types import SimpleNamespace

from serial.tools import list_ports

from iris_gateway import discovery
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


def test_stable_linux_path_survives_usb_product_rename(
    monkeypatch, tmp_path
) -> None:
    device = tmp_path / "ttyACM1"
    device.touch()
    by_path = tmp_path / "by-path"
    by_id = tmp_path / "by-id"
    by_path.mkdir()
    by_id.mkdir()
    topology_link = by_path / "pci-0000-usb-0-7-if00"
    topology_link.symlink_to(device)
    recovery_link = by_id / "usb-ESP-Iris_Recovery-if00"
    recovery_link.symlink_to(device)
    monkeypatch.setattr(
        discovery,
        "_SERIAL_PATH_DIRECTORIES",
        (by_path, by_id),
    )

    recovery_path = discovery._stable_linux_path(str(device))
    recovery_link.unlink()
    (by_id / "usb-ESP-Iris_Normal-if00").symlink_to(device)
    normal_path = discovery._stable_linux_path(str(device))

    assert recovery_path == str(topology_link)
    assert normal_path == str(topology_link)
