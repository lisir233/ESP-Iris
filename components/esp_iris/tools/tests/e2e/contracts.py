from __future__ import annotations

import dataclasses
import struct

TEST_SERVICE_ID = 0x7FFE
STATE_METHOD = 1
LOG_BURST_METHOD = 2
LIFECYCLE_CYCLE_METHOD = 3
MEDIA_CONFIGURE_METHOD = 4
STOP_FOR_FLASH_METHOD = 5
EXERCISE_BOUNDARY_METHOD = 6

STATE_V1 = struct.Struct("<HH21I12s")
LOG_BURST_V1 = struct.Struct("<HHHH")
MEDIA_CONFIGURE_V1 = struct.Struct("<BHB4x")
BOUNDARY_V1 = struct.Struct("<8i")
JOB_REQUEST_V1 = struct.Struct("<BBH")
JOB_ID_V1 = struct.Struct("<I")
POINTER_V1 = struct.Struct("<BBhhHI")


@dataclasses.dataclass(frozen=True, slots=True)
class FixtureState:
    schema: int
    transport: int
    started: bool
    lifecycle: int
    start_count: int
    stop_count: int
    register_count: int
    unregister_count: int
    stdout_records: int
    stderr_records: int
    log_bytes: int
    image_frames: int
    audio_frames: int
    media_errors: int
    jobs_created: int
    jobs_finished: int
    jobs_cancelled: int
    last_error: int
    pointer_count: int
    invalid_frames: int
    log_dropped_bytes: int
    stack_free_min_bytes: int
    internal_heap_used_bytes: int
    last_pointer: bytes

    @classmethod
    def decode(cls, payload: bytes) -> FixtureState:
        if len(payload) != STATE_V1.size:
            raise ValueError(
                f"StateV1 must be {STATE_V1.size} bytes, got {len(payload)}"
            )
        values = STATE_V1.unpack(payload)
        return cls(
            schema=values[0],
            transport=values[1],
            started=bool(values[2]),
            lifecycle=values[3],
            start_count=values[4],
            stop_count=values[5],
            register_count=values[6],
            unregister_count=values[7],
            stdout_records=values[8],
            stderr_records=values[9],
            log_bytes=values[10],
            image_frames=values[11],
            audio_frames=values[12],
            media_errors=values[13],
            jobs_created=values[14],
            jobs_finished=values[15],
            jobs_cancelled=values[16],
            last_error=values[17],
            pointer_count=values[18],
            invalid_frames=values[19],
            log_dropped_bytes=values[20],
            stack_free_min_bytes=values[21],
            internal_heap_used_bytes=values[22],
            last_pointer=values[23],
        )
