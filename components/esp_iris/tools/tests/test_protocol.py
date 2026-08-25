import json
import pathlib
import random
import struct
import zlib

import pytest

from iris_gateway.protocol import (
    MAX_PAYLOAD,
    MAX_WIRE_FRAME,
    Channel,
    ControlType,
    Frame,
    FrameDecoder,
    ProtocolError,
    TlvTag,
    cobs_decode,
    cobs_encode,
    decode_frame,
    decode_tlv,
    encode_frame,
    encode_tlv,
)

VECTORS = pathlib.Path(__file__).resolve().parents[2] / "protocol" / "golden_vectors.json"


def test_frame_round_trip_with_zero_and_ff_bytes() -> None:
    frame = Frame(
        channel=Channel.CONTROL,
        type=ControlType.PING,
        flags=4,
        session_id=0x12345678,
        request_id=9,
        stream_id=10,
        sequence=11,
        payload=bytes(range(256)) + b"\0iris\0",
    )
    decoded = decode_frame(encode_frame(frame)[:-1])
    assert decoded == frame


def test_incremental_decoder_resynchronizes() -> None:
    valid = encode_frame(Frame(channel=0, type=1, payload=b"hello"))
    decoder = FrameDecoder()
    assert decoder.feed(b"\x01" * 4096) == []
    assert decoder.feed(b"\0") == []
    frames = []
    for byte in valid:
        frames.extend(decoder.feed(bytes([byte])))
    assert [frame.payload for frame in frames] == [b"hello"]
    assert decoder.invalid_frames == 1


def test_crc_corruption_is_rejected() -> None:
    wire = bytearray(encode_frame(Frame(channel=0, type=3, payload=b"ping")))
    wire[-3] ^= 0x20
    with pytest.raises(ProtocolError):
        decode_frame(bytes(wire[:-1]))


def test_oversized_payload_and_wire_frame_are_rejected() -> None:
    with pytest.raises(ProtocolError):
        encode_frame(Frame(channel=0, type=3, payload=b"x" * (MAX_PAYLOAD + 1)))
    with pytest.raises(ProtocolError):
        decode_frame(b"\x01" * MAX_WIRE_FRAME)


def test_tlv_unknown_fields_are_skippable() -> None:
    payload = encode_tlv(
        [
            (TlvTag.BOOT_ID, struct.pack("<Q", 42)),
            (0xEE, b"future"),
            (TlvTag.PROJECT_NAME, b"demo"),
        ]
    )
    fields = decode_tlv(payload)
    assert struct.unpack("<Q", fields[TlvTag.BOOT_ID])[0] == 42
    assert fields[0xEE] == b"future"
    assert fields[TlvTag.PROJECT_NAME] == b"demo"


def test_python_codec_matches_normative_vectors() -> None:
    document = json.loads(VECTORS.read_text())
    for vector in document["vectors"]:
        frame = Frame(
            **vector["frame"], payload=bytes.fromhex(vector["payload_hex"])
        )
        wire = bytes.fromhex(vector["wire_hex"])
        assert encode_frame(frame) == wire, vector["name"]
        assert decode_frame(wire[:-1]) == frame, vector["name"]


def _mutate_plain(wire: bytes, offset: int, value: int) -> bytes:
    plain = bytearray(cobs_decode(wire[:-1]))
    plain[offset] = value
    plain[-4:] = struct.pack("<I", zlib.crc32(plain[:-4]) & 0xFFFFFFFF)
    return cobs_encode(plain)


@pytest.mark.parametrize(
    ("offset", "value"),
    [(0, ord("X")), (4, 2), (5, 31), (6, 8), (10, 1), (28, 1)],
)
def test_invalid_header_fields_are_rejected(offset: int, value: int) -> None:
    wire = encode_frame(Frame(channel=0, type=3))
    with pytest.raises(ProtocolError):
        decode_frame(_mutate_plain(wire, offset, value))


@pytest.mark.parametrize("encoded", [b"\x00", b"\x01\x00", b"\x05abc"])
def test_malformed_cobs_is_rejected(encoded: bytes) -> None:
    with pytest.raises(ProtocolError):
        decode_frame(encoded)


@pytest.mark.parametrize(
    "field",
    [
        {"channel": 8},
        {"type": 256},
        {"flags": 1 << 16},
        {"session_id": -1},
        {"request_id": 1 << 32},
        {"stream_id": 1 << 32},
        {"sequence": 1 << 32},
    ],
)
def test_encoder_rejects_out_of_range_fields(field: dict[str, int]) -> None:
    values = {"channel": 0, "type": 1}
    values.update(field)
    with pytest.raises(ProtocolError):
        encode_frame(Frame(**values))


def test_decoder_fuzz_resynchronizes_to_next_valid_frame() -> None:
    randomizer = random.Random(0x1A15)
    expected = encode_frame(Frame(channel=0, type=3, payload=b"recovered"))
    decoder = FrameDecoder()
    for _ in range(1000):
        noise = randomizer.randbytes(randomizer.randrange(0, 128))
        frames = decoder.feed(noise + b"\x00" + expected)
        assert frames[-1].payload == b"recovered"
