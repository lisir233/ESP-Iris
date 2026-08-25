import struct
import zlib

import pytest

from iris_gateway.media import JPEG, PNG, RGB565, RGB888, encode_media_image


def _decode_png_rgb(data: bytes) -> tuple[int, int, bytes]:
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    offset = 8
    width = height = 0
    compressed = bytearray()
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        name = data[offset + 4 : offset + 8]
        body = data[offset + 8 : offset + 8 + size]
        offset += 12 + size
        if name == b"IHDR":
            width, height = struct.unpack_from(">II", body)
        elif name == b"IDAT":
            compressed.extend(body)
        elif name == b"IEND":
            break
    rows = zlib.decompress(compressed)
    row_size = width * 3
    assert len(rows) == height * (row_size + 1)
    return width, height, b"".join(
        rows[y * (row_size + 1) + 1 : (y + 1) * (row_size + 1)]
        for y in range(height)
    )


def test_rgb565_screenshot_is_encoded_as_real_png() -> None:
    raw = struct.pack("<HH", 0xF800, 0x07E0)
    image = encode_media_image(
        {"width": 2, "height": 1, "stride": 4, "format": RGB565}, raw
    )
    assert image.content_type == "image/png"
    assert image.description["format"] == PNG
    assert image.description["source_format"] == RGB565
    assert image.description["source_stride"] == 4
    assert _decode_png_rgb(image.data) == (2, 1, b"\xff\x00\x00\x00\xff\x00")


def test_rgb888_stride_padding_is_removed() -> None:
    image = encode_media_image(
        {"width": 1, "height": 2, "stride": 4, "format": RGB888},
        b"\x01\x02\x03x\x04\x05\x06y",
    )
    assert _decode_png_rgb(image.data) == (1, 2, b"\x01\x02\x03\x04\x05\x06")


def test_existing_png_and_jpeg_are_preserved_by_signature() -> None:
    png = b"\x89PNG\r\n\x1a\nrest"
    jpeg = b"\xff\xd8rest"
    assert encode_media_image({"format": JPEG}, png).data == png
    assert encode_media_image({"format": PNG}, jpeg).content_type == "image/jpeg"


def test_invalid_encoded_payload_is_rejected() -> None:
    with pytest.raises(ValueError, match="invalid signature"):
        encode_media_image({"format": PNG}, b"not png")
