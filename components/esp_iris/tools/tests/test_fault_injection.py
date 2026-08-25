import random

from iris_gateway.protocol import Frame, FrameDecoder, encode_frame


def test_decoder_recovers_from_deterministic_fragmentation_and_corruption() -> None:
    random_source = random.Random(0x1A15)
    expected = [Frame(channel=0, type=3, sequence=index + 1, payload=bytes([index])) for index in range(32)]
    stream = bytearray()
    for index, frame in enumerate(expected):
        if index % 5 == 0:
            stream.extend(b"\x02\xff\x00")
        stream.extend(encode_frame(frame))
    decoder = FrameDecoder()
    decoded = []
    offset = 0
    while offset < len(stream):
        size = random_source.randint(1, 19)
        decoded.extend(decoder.feed(bytes(stream[offset : offset + size])))
        offset += size
    assert decoded == expected
    assert decoder.invalid_frames >= 1


def test_decoder_bounds_unterminated_garbage_before_valid_frame() -> None:
    decoder = FrameDecoder()
    assert decoder.feed(b"\xff" * 8192) == []
    valid = Frame(channel=0, type=3, sequence=1, payload=b"recovered")
    assert decoder.feed(b"\x00" + encode_frame(valid)) == [valid]
    assert decoder.invalid_frames == 1

