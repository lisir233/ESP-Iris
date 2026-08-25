from __future__ import annotations

import dataclasses
import hashlib
import struct

ESP_IMAGE_MAGIC = 0xE9
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP32S31_CHIP_ID = 0x0020


@dataclasses.dataclass(frozen=True, slots=True)
class FirmwareImage:
    sha256: str
    size: int
    chip_id: int
    project_name: str
    version: str
    elf_sha256: str

    def as_dict(self) -> dict[str, str | int]:
        return dataclasses.asdict(self)


def _cstring(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def inspect_firmware_image(image: bytes) -> FirmwareImage:
    if len(image) < 112 or image[0] != ESP_IMAGE_MAGIC:
        raise ValueError("not an ESP application image")
    chip_id = struct.unpack_from("<H", image, 12)[0]
    if chip_id != ESP32S31_CHIP_ID:
        raise ValueError(
            f"firmware chip id 0x{chip_id:04x} is not ESP32-S31 (0x{ESP32S31_CHIP_ID:04x})"
        )
    app_desc_offset = 24 + 8
    magic = struct.unpack_from("<I", image, app_desc_offset)[0]
    if magic != ESP_APP_DESC_MAGIC:
        raise ValueError("ESP application descriptor is missing from first segment")
    version = _cstring(image[app_desc_offset + 16 : app_desc_offset + 48])
    project_name = _cstring(image[app_desc_offset + 48 : app_desc_offset + 80])
    if not project_name or not version:
        raise ValueError("firmware project name and version must be present")
    # esp_app_desc_t places the full application ELF SHA-256 after the IDF
    # version field. Older synthetic test images may omit the tail.
    elf_sha_offset = app_desc_offset + 144
    elf_sha256 = (
        image[elf_sha_offset : elf_sha_offset + 32].hex()
        if len(image) >= elf_sha_offset + 32
        else ""
    )
    return FirmwareImage(
        sha256=hashlib.sha256(image).hexdigest(),
        size=len(image),
        chip_id=chip_id,
        project_name=project_name,
        version=version,
        elf_sha256=elf_sha256,
    )


__all__ = ["ESP32S31_CHIP_ID", "FirmwareImage", "inspect_firmware_image"]
