from __future__ import annotations

import os
import pathlib
from dataclasses import dataclass

_SERIAL_PATH_DIRECTORIES = (
    pathlib.Path("/dev/serial/by-path"),
    pathlib.Path("/dev/serial/by-id"),
)


@dataclass(frozen=True)
class IrisUsbDevice:
    path: str
    device: str
    vid: int
    pid: int
    serial_number: str
    product: str = ""
    transport: str = "usb"
    location: str = ""


def _stable_linux_path(device: str) -> str:
    if os.name != "posix":
        return device
    target = os.path.realpath(device)
    # Prefer the physical USB topology path. ESP-Iris recovery and normal
    # firmware intentionally expose different product strings, so Linux gives
    # them different by-id names. A supervisor opened through the old by-id
    # link cannot reconnect after the device re-enumerates. The by-path link
    # remains stable across that transition; by-id is only a fallback for
    # systems that do not expose by-path links.
    for directory in _SERIAL_PATH_DIRECTORIES:
        if not directory.is_dir():
            continue
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
                    location=getattr(port, "location", None) or "",
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
