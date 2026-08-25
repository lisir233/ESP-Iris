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


def _stable_linux_path(device: str) -> str:
    directory = pathlib.Path("/dev/serial/by-id")
    if os.name != "posix" or not directory.is_dir():
        return device
    target = os.path.realpath(device)
    for candidate in sorted(directory.iterdir()):
        if os.path.realpath(candidate) == target and "ESP-Iris" in candidate.name:
            return str(candidate)
    return device


def discover_iris_usb_devices() -> list[IrisUsbDevice]:
    from serial.tools import list_ports

    devices = []
    for port in list_ports.comports():
        if port.vid == 0x303A and (
            port.pid == 0x4002 or "ESP-Iris" in (port.product or "")
        ):
            devices.append(
                IrisUsbDevice(
                    path=_stable_linux_path(port.device),
                    device=port.device,
                    vid=port.vid,
                    pid=port.pid or 0,
                    serial_number=port.serial_number or "",
                    product=port.product or "",
                )
            )
    return sorted(devices, key=lambda item: item.path)


def discover_iris_usb_ports() -> list[str]:
    return sorted({device.path for device in discover_iris_usb_devices()})
