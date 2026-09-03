from __future__ import annotations

import hashlib
import struct

import pytest

from iris_gateway.firmware import (
    ESP32S31_CHIP_ID,
    ESP_APP_DESC_MAGIC,
    inspect_firmware_image,
)


def firmware_image(project: str = "esp-iris-template", version: str = "3.6.0") -> bytes:
    image = bytearray(256)
    image[0] = 0xE9
    image[1] = 1
    struct.pack_into("<H", image, 12, ESP32S31_CHIP_ID)
    descriptor = 32
    struct.pack_into("<I", image, descriptor, ESP_APP_DESC_MAGIC)
    image[descriptor + 16 : descriptor + 16 + len(version)] = version.encode()
    image[descriptor + 48 : descriptor + 48 + len(project)] = project.encode()
    return bytes(image)


def test_inspect_esp32s31_firmware_descriptor() -> None:
    image = firmware_image()
    result = inspect_firmware_image(image)
    assert result.project_name == "esp-iris-template"
    assert result.version == "3.6.0"
    assert result.chip_id == ESP32S31_CHIP_ID
    assert result.sha256 == hashlib.sha256(image).hexdigest()


def test_reject_wrong_chip_before_ota() -> None:
    image = bytearray(firmware_image())
    struct.pack_into("<H", image, 12, 0x0009)
    with pytest.raises(ValueError, match="not ESP32-S31"):
        inspect_firmware_image(bytes(image))
