import ctypes
import json
import pathlib
import shutil
import subprocess

import pytest
from iris_gateway.protocol import (
    MAX_PAYLOAD,
    MAX_WIRE_FRAME,
    Frame,
    decode_frame,
    encode_frame,
)

COMPONENT = pathlib.Path(__file__).resolve().parents[2]
VECTORS = COMPONENT / "protocol" / "golden_vectors.json"


class CWireHeader(ctypes.Structure):
    _fields_ = [
        ("channel", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("flags", ctypes.c_uint16),
        ("session_id", ctypes.c_uint32),
        ("request_id", ctypes.c_uint32),
        ("stream_id", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
        ("payload_size", ctypes.c_uint32),
    ]


class CDecodedFrame(ctypes.Structure):
    _fields_ = [("header", CWireHeader), ("payload", ctypes.c_void_p)]


@pytest.fixture(scope="module")
def c_codec(tmp_path_factory: pytest.TempPathFactory):
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for device/PC codec compatibility")
    output = tmp_path_factory.mktemp("iris-c-codec") / "libiris_codec.so"
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-shared",
            "-fPIC",
            "-I",
            str(pathlib.Path(__file__).parent / "host_include"),
            "-I",
            str(COMPONENT / "include"),
            "-I",
            str(COMPONENT / "src"),
            str(COMPONENT / "src" / "esp_iris_codec.c"),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    library = ctypes.CDLL(str(output))
    library.iris_frame_encode.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(CWireHeader),
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.iris_frame_encode.restype = ctypes.c_int32
    library.iris_frame_decode_in_place.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.POINTER(CDecodedFrame),
    ]
    library.iris_frame_decode_in_place.restype = ctypes.c_int32
    return library


def _frame(vector: dict[str, object]) -> Frame:
    fields = vector["frame"]
    assert isinstance(fields, dict)
    payload_hex = vector["payload_hex"]
    assert isinstance(payload_hex, str)
    return Frame(**fields, payload=bytes.fromhex(payload_hex))


def _c_encode(c_codec, frame: Frame) -> bytes:
    header = CWireHeader(
        channel=frame.channel,
        type=frame.type,
        flags=frame.flags,
        session_id=frame.session_id,
        request_id=frame.request_id,
        stream_id=frame.stream_id,
        sequence=frame.sequence,
        payload_size=len(frame.payload),
    )
    output = (ctypes.c_uint8 * MAX_WIRE_FRAME)()
    payload = (ctypes.c_uint8 * len(frame.payload)).from_buffer_copy(frame.payload)
    output_size = ctypes.c_size_t()
    result = c_codec.iris_frame_encode(
        output,
        len(output),
        ctypes.byref(header),
        payload,
        len(payload),
        ctypes.byref(output_size),
    )
    assert result == 0
    return bytes(output[: output_size.value])


def _c_decode(c_codec, wire: bytes) -> Frame:
    encoded = wire[:-1]
    buffer = (ctypes.c_uint8 * len(encoded)).from_buffer_copy(encoded)
    decoded = CDecodedFrame()
    assert c_codec.iris_frame_decode_in_place(
        buffer, len(buffer), ctypes.byref(decoded)
    ) == 0
    header = decoded.header
    return Frame(
        channel=header.channel,
        type=header.type,
        flags=header.flags,
        session_id=header.session_id,
        request_id=header.request_id,
        stream_id=header.stream_id,
        sequence=header.sequence,
        payload=ctypes.string_at(decoded.payload, header.payload_size),
    )


def _c_decode_status(c_codec, wire: bytes) -> int:
    encoded = wire[:-1] if wire.endswith(b"\x00") else wire
    buffer = (ctypes.c_uint8 * len(encoded)).from_buffer_copy(encoded)
    decoded = CDecodedFrame()
    return c_codec.iris_frame_decode_in_place(
        buffer, len(buffer), ctypes.byref(decoded)
    )


def test_c_and_python_match_normative_vectors(c_codec) -> None:
    document = json.loads(VECTORS.read_text())
    assert document["schema_version"] == 1
    assert document["protocol_version"] == 1
    for vector in document["vectors"]:
        frame = _frame(vector)
        expected = bytes.fromhex(vector["wire_hex"])
        assert encode_frame(frame) == expected, vector["name"]
        assert _c_encode(c_codec, frame) == expected, vector["name"]
        assert decode_frame(expected[:-1]) == frame, vector["name"]
        assert _c_decode(c_codec, expected) == frame, vector["name"]


def test_c_and_python_support_maximum_payload(c_codec) -> None:
    frame = Frame(
        channel=0,
        type=3,
        session_id=1,
        sequence=1,
        payload=bytes(index & 0xFF for index in range(MAX_PAYLOAD)),
    )
    python_wire = encode_frame(frame)
    assert len(python_wire) <= MAX_WIRE_FRAME
    assert _c_encode(c_codec, frame) == python_wire
    assert _c_decode(c_codec, python_wire) == frame


def test_c_decoder_rejects_corrupt_truncated_and_oversized_frames(c_codec) -> None:
    corrupt = bytearray(encode_frame(Frame(channel=0, type=3, payload=b"ping")))
    corrupt[-3] ^= 0x20
    assert _c_decode_status(c_codec, bytes(corrupt)) != 0
    assert _c_decode_status(c_codec, b"\x05abc") != 0
    assert _c_decode_status(c_codec, b"\x01" * MAX_WIRE_FRAME) != 0
