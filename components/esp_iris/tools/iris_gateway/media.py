from __future__ import annotations

import dataclasses
import struct
import zlib
from typing import Any


RGB565 = 1
RGB888 = 2
JPEG = 3
PNG = 4


@dataclasses.dataclass(frozen=True, slots=True)
class EncodedImage:
    description: dict[str, int]
    data: bytes
    content_type: str
    extension: str


def _png_chunk(name: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + name
        + data
        + struct.pack(">I", zlib.crc32(name + data) & 0xFFFFFFFF)
    )


def _encode_rgb_png(width: int, height: int, rgb: bytes) -> bytes:
    if len(rgb) != width * height * 3:
        raise ValueError("RGB image size does not match its dimensions")
    rows = bytearray()
    row_size = width * 3
    for y in range(height):
        rows.append(0)
        start = y * row_size
        rows.extend(rgb[start : start + row_size])
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", header)
        + _png_chunk(b"IDAT", zlib.compress(bytes(rows), 6))
        + _png_chunk(b"IEND", b"")
    )


def _raw_rgb(description: dict[str, Any], data: bytes) -> tuple[int, int, bytes]:
    width = int(description.get("width", 0))
    height = int(description.get("height", 0))
    format_ = int(description.get("format", 0))
    bytes_per_pixel = 2 if format_ == RGB565 else 3 if format_ == RGB888 else 0
    stride = int(description.get("stride", 0)) or width * bytes_per_pixel
    if width <= 0 or height <= 0 or bytes_per_pixel == 0:
        raise ValueError("unsupported or empty raw image description")
    if stride < width * bytes_per_pixel or len(data) != stride * height:
        raise ValueError("raw image size does not match width, height, and stride")

    rgb = bytearray(width * height * 3)
    output = 0
    for y in range(height):
        row = memoryview(data)[y * stride : y * stride + width * bytes_per_pixel]
        if format_ == RGB565:
            for x in range(width):
                value = row[x * 2] | row[x * 2 + 1] << 8
                red = (value >> 11) & 0x1F
                green = (value >> 5) & 0x3F
                blue = value & 0x1F
                rgb[output : output + 3] = bytes(
                    ((red << 3) | (red >> 2),
                     (green << 2) | (green >> 4),
                     (blue << 3) | (blue >> 2))
                )
                output += 3
        else:
            size = width * 3
            rgb[output : output + size] = row
            output += size
    return width, height, bytes(rgb)


def encode_media_image(description: dict[str, Any], data: bytes) -> EncodedImage:
    """Return browser-safe PNG/JPEG bytes for an ESP-Iris image payload."""

    source = {key: int(value) for key, value in description.items()}
    format_ = int(source.get("format", 0))
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        encoded = {**source, "format": PNG}
        return EncodedImage(encoded, data, "image/png", "png")
    if data.startswith(b"\xff\xd8"):
        encoded = {**source, "format": JPEG}
        return EncodedImage(encoded, data, "image/jpeg", "jpg")
    if format_ in (RGB565, RGB888):
        width, height, rgb = _raw_rgb(source, data)
        encoded = {
            **source,
            "width": width,
            "height": height,
            "stride": width * 3,
            "format": PNG,
            "source_format": format_,
            "source_stride": int(source.get("stride", 0)),
        }
        return EncodedImage(
            encoded, _encode_rgb_png(width, height, rgb), "image/png", "png"
        )
    if format_ == PNG:
        raise ValueError("PNG payload has an invalid signature")
    if format_ == JPEG:
        raise ValueError("JPEG payload has an invalid signature")
    raise ValueError(f"unsupported ESP-Iris image format: {format_}")


__all__ = ["EncodedImage", "JPEG", "PNG", "RGB565", "RGB888", "encode_media_image"]
