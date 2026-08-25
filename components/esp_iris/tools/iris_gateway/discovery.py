from __future__ import annotations

import os
import pathlib
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class IrisUsbDevice:
    path: str
    device: str
    vid: int
    pid: int
    serial_number: str
    product: str = ""
    transport: str = "usb"


def _stable_linux_path(device: str) -> str:
    directory = pathlib.Path("/dev/serial/by-id")
    if os.name != "posix" or not directory.is_dir():
        return device
    target = os.path.realpath(device)
    for candidate in sorted(directory.iterdir()):
        if os.path.realpath(candidate) == target:
            return str(candidate)
    return device


def discover_iris_usb_devices(
    *, include_usb_serial_jtag: bool = False
) -> list[IrisUsbDevice]:
    from serial.tools import list_ports

    devices = []
    for port in list_ports.comports():
        is_iris_cdc = port.vid == 0x303A and (
            port.pid == 0x4002 or "ESP-Iris" in (port.product or "")
        )
        is_usb_serial_jtag = (
            include_usb_serial_jtag
            and port.vid == 0x303A
            and port.pid == 0x1001
            and "USB JTAG/serial debug unit" in (port.product or "")
        )
        if is_iris_cdc or is_usb_serial_jtag:
            devices.append(
                IrisUsbDevice(
                    path=_stable_linux_path(port.device),
                    device=port.device,
                    vid=port.vid,
                    pid=port.pid or 0,
                    serial_number=port.serial_number or "",
                    product=port.product or "",
                    transport=(
                        "usb_serial_jtag" if is_usb_serial_jtag else "usb"
                    ),
                )
            )
    return sorted(devices, key=lambda item: item.path)


def discover_iris_usb_ports(
    *, include_usb_serial_jtag: bool = False
) -> list[str]:
    return sorted(
        {
            device.path
            for device in discover_iris_usb_devices(
                include_usb_serial_jtag=include_usb_serial_jtag
            )
        }
    )
