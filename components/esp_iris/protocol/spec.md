# ESP-Iris protocol v1

Status: milestones M1-M11 are implemented. The v1 wire envelope is frozen;
newer services use the existing request ID, stream ID, credit and channel
fields.

## Link

Normal firmware compiles exactly one link: application USB CDC0, USB
Serial/JTAG, or a raw TCP server. USB serial channels contain only ESP-Iris
binary frames. Baud rate and line coding have no protocol meaning. Application
USB DTR open/close delimits a PC link session, but never requests reset or ROM
download mode.

USB Serial/JTAG uses transport value `3` (`1` is application USB CDC0 and `2`
is TCP). Its ESP-IDF public connection state describes cable/SOF presence, not
whether a process has the serial endpoint open. It therefore repeats HELLO
once per second even after HELLO_ACK. A Gateway reopen may re-acknowledge that
HELLO and joins the existing physical session; only cable loss or reboot
creates a new session ID. Repeated HELLO_ACK remains idempotent. Firmware
disables the USB Serial/JTAG DTR/RTS reset function while this transport owns
the serial channel, restoring its previous value on `esp_iris_stop()`.

The TCP server listens on `0.0.0.0:19772` by default and accepts one physical
session. ESP-Iris does not initialize Wi-Fi or provision credentials. The PC
Web API is a separate process and listens only on a loopback address.

## Envelope

Each frame is encoded as:

```text
COBS(header || payload || crc32) || 0x00
```

All integers are little endian. CRC32 is IEEE CRC-32 as implemented by zlib,
over the decoded header and payload. The delimiter is not covered. Receivers
drop invalid bytes until the next `0x00` delimiter.

The decoded 32-byte header is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `IRIS` |
| 4 | 1 | protocol version (`1`) |
| 5 | 1 | header size (`32`) |
| 6 | 1 | channel |
| 7 | 1 | type, scoped by channel |
| 8 | 2 | flags |
| 10 | 2 | reserved, must be zero |
| 12 | 4 | session ID |
| 16 | 4 | request ID, or zero |
| 20 | 4 | stream ID, or zero |
| 24 | 4 | per-channel sequence |
| 28 | 4 | payload size |

Maximum payload is 4000 bytes and maximum encoded frame, including delimiter,
is 4096 bytes. A large object uses service-specific OPEN/DATA/CLOSE frames;
it is never placed in one oversized envelope.

Channels are `CONTROL=0`, `LOG=1`, `EVENT=2`, `SCREEN=3`, `IMAGE=4`,
`AUDIO=5`, `OTA=6`, `CRASH=7`, and `FILE=8`. FILE is sent only when both peers
recognize `CAP_FILE` (capability bit 13). `CAP_OTA_PROJECT_NAME_MATCH`
(capability bit 14) advertises that the running firmware requires an OTA
image's project name to match its own. An absent bit means that cross-project
updates are allowed. Unknown types on a known channel produce a CONTROL ERROR
when a response is possible. A future protocol version must use capability
negotiation rather than silently reinterpreting an existing type.

## Control session

The device chooses a nonzero random session ID for every physical connection
and repeats HELLO once a second until HELLO_ACK. The PC echoes that session ID
in every frame. Old-session frames are discarded.

After the first HELLO_ACK in a session, the device emits exactly one BOOT event
for that session. A repeated HELLO or HELLO_ACK must not create another BOOT
event. BOOT is deliberately replayed after a link reconnect so a new PC Hub
can recover boot metadata; it does not by itself mean the device rebooted.
Consumers compare `boot_id` to detect a real boot and `session_id` to detect a
new physical link session.

After HELLO_ACK, replay order is BOOT, LINK_READY, optional
PREVIOUS_BOOT_CRASH, optional CORE_DUMP_AVAILABLE and optional HEALTHY.
`esp_iris_mark_healthy()` updates replayable lifecycle state. A planned restart
event records local intent; products with crash-loop recovery override
`esp_iris_platform_mark_planned_restart()` to persist that intent. Event type
`0x02` is reserved and must not be reinterpreted.

Implemented control types:

| Type | Value | Payload |
|---|---:|---|
| HELLO | `0x01` | TLV device description |
| HELLO_ACK | `0x02` | empty, or auth nonce + proof |
| PING/PONG | `0x03/0x04` | opaque echoed bytes |
| TIME_SYNC_REQUEST | `0x05` | host monotonic `t1_ns: u64` |
| TIME_SYNC_RESPONSE | `0x06` | `t1_ns, device_d2_us, device_d3_us: u64` |
| STATUS_REQUEST | `0x07` | empty |
| STATUS_RESPONSE | `0x08` | TLV status |
| CREDIT | `0x09` | `channel:u8, reserved[3], bytes:u32` |
| REQUEST/RESPONSE | `0x10/0x11` | bounded binary RPC |
| CANCEL | `0x12` | `job_id:u32` |
| JOB_QUERY/JOB_STATUS | `0x13/0x14` | job ID / fixed job status |
| RESTART | `0x15` | `delay_ms:u32` |
| AUTH_RESULT | `0x16` | `accepted:u8` |
| ERROR | `0x7f` | `esp_err:u32, channel:u8, type:u8, reserved:u16` |

CONTROL and reliable EVENT traffic are not charged against media/log credit.
The PC must grant LOG credit before the device transmits LOG records.

## TLV

Control and event metadata use:

```text
tag:u8 || length:u16 || value[length]
```

The value's scalar encoding is defined by the tag. Unknown tags are skipped.
Strings are UTF-8 without a terminating NUL. Device ID is 16 raw UUID bytes;
boot ID and capabilities are `u64`.

The stable device ID is generated once and stored under NVS namespace
`esp_iris`. The boot ID is random on every boot. Neither is derived solely
from the MAC address.

STATUS includes lifecycle state, link/invalid-frame counters, minimum worker
stack headroom, maximum active worker-loop time, startup internal-heap delta
and static Iris bytes. The PC reports `internal_total_bytes` as static bytes
plus Iris heap usage. Optional service allocations created after start are
included. Mirror buffers are released on stop; registered RPC/screen metadata
and retained jobs stay bounded by Kconfig.

## Logs

LOG RECORD (`type=0x01`) payload:

```text
monotonic_us:u64
dropped_total:u32
source:u8             # 1 stdout, 2 stderr
flags:u8
length:u16
data[length]
```

The VFS writer is nonblocking. A full device ring drops the oldest records and
increments `dropped_total`; writes still report the original byte count to the
caller. The protocol implementation itself never calls printf or ESP_LOG.

## Time

Device event ordering always uses `esp_timer_get_time()` in microseconds. The
PC estimates offset with four timestamps and stores device monotonic time,
host receive time, estimated wall time, and uncertainty. Device wall clock is
not an ordering authority.

Every PC event carries an `event_id` derived from device ID, boot ID, device
monotonic time and per-channel sequence. A Hub classifies a new session as
`connected`, `reconnected` or `rebooted` by comparing the last boot ID seen for
that device. Sequence duplicates and backwards frames on a live session are
dropped.

## Crash evidence

The CRASH channel is read-only. It never erases a coredump or writes a crash
partition.

| Type | Value | Payload |
|---|---:|---|
| METADATA_REQUEST | `0x01` | empty |
| METADATA_RESPONSE | `0x02` | TLV crash report |
| READ_REQUEST | `0x03` | `offset:u32, maximum:u16, reserved:u16` |
| READ_RESPONSE | `0x04` | `offset:u32, total_size:u32, data[]` |

Metadata always reports reset reason and whether it represents a previous-boot
crash. When Flash coredump support is compiled and a valid coredump partition
is present, it also reports retained size, panic reason, coredump ELF SHA and
the maximum chunk size. READ responses set STREAM_END on the final chunk and
reuse the device RX frame buffer, so no media-sized or full-coredump allocation
is required.

The PC permits evidence download even when the embedded ELF SHA is incomplete,
but sets `decode_eligible=true` only when a complete 64-character SHA matches
the running firmware identity. Decoding against a nonmatching ELF is outside
the protocol contract.

## File service

The optional FILE channel exposes only application-registered logical volumes.
ESP-Iris never exports `/`, NVS, OTA partitions, coredumps, its log VFS, or any
mount automatically. A target is encoded as a logical volume ID plus a canonical
UTF-8 relative path:

```text
volume_length:u8, reserved:u8, path_length:u16
volume[volume_length], path[path_length]
```

Volume IDs contain 1-15 ASCII letters, digits, `_`, or `-`. The empty path means
the volume root. Nonempty paths cannot start or end with `/`, contain an empty,
`.` or `..` component, contain `\`, NUL, or control bytes, or exceed 255 encoded
bytes. ESP-IDF's supported LittleFS, SPIFFS, and FATFS VFS backends do not expose
symbolic links; unsupported file kinds are omitted or rejected.

Every FILE response begins with `status:u16, reserved:u16`. Status is a stable
protocol value, not a platform `errno`: `OK=0`, `INVALID_ARGUMENT=1`,
`NOT_FOUND=2`, `NOT_DIRECTORY=3`, `NOT_FILE=4`, `READ_ONLY=5`, `BUSY=6`,
`NO_MEMORY=7`, `IO=8`, `NOT_SUPPORTED=9`, `CONFLICT=10`, `EXISTS=11`,
`NOT_EMPTY=12`, `NO_SPACE=13`, and `HASH_MISMATCH=14`.

Implemented FILE types are:

| Type | Value | Request / response payload after status |
|---|---:|---|
| VOLUMES_REQUEST/RESPONSE | `0x01/0x02` | empty / `chunk_max:u16, path_max:u16, count:u8, reserved[3]`, then volume records |
| STAT_REQUEST/RESPONSE | `0x03/0x04` | path target / metadata |
| LIST_OPEN/OPENED | `0x05/0x06` | path target / `stream_id:u32` |
| LIST_NEXT/DATA | `0x07/0x08` | empty / page header and at most one entry |
| CLOSE/CLOSE_RESPONSE | `0x09/0x0a` | empty / empty |
| READ_OPEN/OPENED | `0x0b/0x0c` | path target / stream metadata |
| READ/DATA | `0x0d/0x0e` | read range / offset, total and bytes |
| WRITE_OPEN/OPENED | `0x0f/0x10` | path target plus write declaration / stream ID and chunk maximum |
| WRITE/ACK | `0x11/0x12` | strict offset and bytes / committed offset |
| COMMIT/COMMIT_RESPONSE | `0x13/0x14` | SHA-256 / final metadata |
| ABORT/ABORT_RESPONSE | `0x15/0x16` | empty / empty |
| MKDIR/MKDIR_RESPONSE | `0x17/0x18` | path target / metadata |
| DELETE/DELETE_RESPONSE | `0x19/0x1a` | path target / empty |
| RENAME/RENAME_RESPONSE | `0x1b/0x1c` | same-volume source and destination / metadata |
| WRITE_STATUS/WRITE_STATUS_RESPONSE | `0x1d/0x1e` | empty / resumable write state |

A volume record is `id_length:u8, reserved:u8, capabilities:u16, id[]`.
Capabilities are `READ=bit0`, `LIST=bit1`, `MTIME=bit2`, `WRITE=bit3`,
`DELETE=bit4`, `MKDIR=bit5`, `RENAME=bit6`, `ATOMIC_REPLACE=bit7`, and
`HASH=bit8`. `WRITE` always implies `HASH`; overwrite is rejected unless the
product also declares `ATOMIC_REPLACE`. Common metadata is:

```text
kind:u8              # 1 regular file, 2 directory
reserved:u8
reserved:u16
size:u64
mtime_s:u64          # zero when unavailable
opaque_etag:u64
```

LIST_OPEN returns the same nonzero stream ID in the envelope and payload and sets
STREAM_BEGIN. LIST_NEXT carries that ID in the envelope. LIST_DATA is
`end:u8, count:u8` after status; count is 0 or 1. An entry replaces metadata's
first reserved byte with `name_length:u8` and appends `name[name_length]`.
The terminal empty page sets `end bit0` and STREAM_END. Directory order and
cursor replay are not snapshot semantics. CLOSE releases the stream.

READ_OPEN similarly returns `stream_id:u32, total_size:u64, mtime_s:u64,
opaque_etag:u64, chunk_max:u16, reserved:u16` and sets STREAM_BEGIN. READ is
`offset:u64, maximum:u16, reserved:u16`; DATA is `flags:u16, offset:u64,
total_size:u64, data[]` after status. Requests are stop-and-wait and offsets are
64-bit. The last DATA sets STREAM_END and flags bit 0. Files are read by the
dedicated bounded, low-priority file task; only the Iris worker writes frames.

WRITE_OPEN appends the following declaration to a nonempty path target:

```text
total_size:u64
if_match_etag:u64
flags:u16              # bit0 overwrite, bit1 if-match is present
reserved:u16
```

It creates an exclusive temporary file in the target's existing parent
directory. Creating an existing target returns `EXISTS`; overwriting requires
both the overwrite flag and the volume's `ATOMIC_REPLACE` capability. If-match
compares the opaque target ETag before opening and the device checks the target
again immediately before replacement. WRITE_OPENED returns
`stream_id:u32, chunk_max:u16, reserved:u16` and sets STREAM_BEGIN.

WRITE is `offset:u64, data_size:u16, reserved:u16, data[data_size]`. The offset
must equal the current committed offset, data must not exceed either the
declared total or chunk maximum, and ACK returns `committed_offset:u64`.
Requests are stop-and-wait. After an ACK timeout, the host queries WRITE_STATUS
instead of blindly retransmitting.

COMMIT contains exactly the SHA-256 of the declared file. The device requires
the exact declared byte count and hash, verifies that the destination did not
change, then performs `fsync`, close, and same-directory rename. Only after the
rename does it return final metadata and STREAM_END. It never degrades an
advertised atomic replacement into an in-place write. The temporary file needs
additional free space and is removed by ABORT, session loss, write failure, or
failed commit.

WRITE_STATUS uses the write stream ID and returns:

```text
committed_offset:u64
expected_size:u64
state:u8               # 1 active, 2 committed, 3 aborted
reserved[3]
result:u16              # stable FILE status for terminal state
reserved:u16
```

The device retains the last terminal receipt for the session so a host can
resolve a lost COMMIT response. A later WRITE_OPEN replaces that receipt.

MKDIR creates exactly one directory. DELETE removes a regular file or an empty
directory; recursive deletion is not defined. RENAME carries
`volume_length:u8, reserved:u8, source_length:u16, destination_length:u16,
reserved:u16, volume[], source[], destination[]`, stays within one logical
volume, and never overwrites an existing destination. The volume root cannot
be written, renamed, or deleted, and a directory cannot be moved below itself.
Only one LIST, READ, or WRITE handle is active
at a time; unrelated metadata operations remain bounded and mutations return
BUSY while a handle is active.

## RPC and jobs

CONTROL REQUEST payload:

```text
service_id:u16, method_id:u16, deadline_ms:u32
body_size:u16, reserved:u16, body[body_size]
```

CONTROL RESPONSE repeats service/method followed by
`result:i32, body_size:u16, reserved:u16, body[]`. Bodies are capped by
`CONFIG_ESP_IRIS_RPC_BODY_BYTES`; handlers receive binary spans and run on
the Iris worker, so they must not block. A response that finishes after its
relative deadline is returned as timeout. Reusing the last request ID in one
session is deterministically rejected.

Long operations use a bounded job record:

```text
job_id:u32, kind:u16, state:u8, cancel_requested:u8
progress_permille:u16, reserved:u16, result:i32
```

JOB_QUERY and CANCEL return JOB_STATUS. Updates are also emitted as reliable
EVENT JOB_UPDATE (`0x20`). Cancellation is cooperative; disconnect requests
cancellation of every running session job and aborts in-progress OTA.

## Screenshot and unified media

The 16-byte media description is:

```text
x:u16, y:u16, width:u16, height:u16
stride:u32, format:u16, quality:u16
```

SCREEN OPEN supplies a requested description. OPENED returns the negotiated
description plus `total_size:u32`. READ uses
`offset:u32, maximum:u16, reserved:u16`; DATA returns
`offset:u32, total_size:u32, data[]`. The backend fills only the requested
chunk, so Iris never requires a full framebuffer or PSRAM. CLOSE releases the
capture.

SCREEN, IMAGE and AUDIO share MIRROR_START/MIRROR_STOP and DATA types.
MIRROR_START appends `fps:u16, reserved:u16` to the description. It is always
off after boot and link loss. Each active channel owns one bounded latest
chunk. A newer application submission overwrites an unsent chunk and
increments the dropped counter instead of creating backlog.

When SCREEN has a registered pull backend, MIRROR_START reuses that backend.
Raw RGB565/RGB888 frames are read as whole-scanline tiles no larger than
`CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES`; each tile description carries its
absolute `y` and tile `height`. All tiles in one frame share `frame_id`, while
the envelope `stream_id` remains the stable value negotiated by MIRROR_STATE.
This path does not allocate a second full framebuffer.

Unsolicited media DATA uses:

```text
monotonic_us:u64, frame_id:u32, dropped:u32
flags:u16, data_size:u16, description[16], data[data_size]
```

Each media channel has independent byte credit. CONTROL/EVENT responses are
scheduled before media, preventing congestion from starving control. The PC
Hub receives one physical stream and fans it out to bounded local queues.

## TCP pairing

USB is always auth mode 0. With `CONFIG_ESP_IRIS_TCP_PAIRING`, the device
stores a random 32-byte token in the existing `esp_iris` NVS namespace and
advertises auth mode 1 plus a fresh 32-byte challenge in HELLO. The token is
never sent on the link.

The PC HELLO_ACK is `client_nonce[16] || hmac_sha256[32]`. The HMAC key is the
token and its message is:

```text
"ESP-Iris-auth-v1" || device_id[16] || boot_id:u64 || session_id:u32
|| challenge[32] || client_nonce[16]
```

The device uses PSA Crypto, constant-time comparison, a fresh challenge per
physical session and a bounded delay after failure. Only a successful
AUTH_RESULT makes the session ready; before that, every frame except
HELLO_ACK is discarded without reaching status, crash, RPC, media or OTA
handlers. Token get/rotate is intended for a product-owned secure
provisioning surface; Iris never logs the token.

## OTA and recovery

OTA BEGIN payload is:

```text
total_size:u32, sha256[32], project_len:u8, version_len:u8
reserved:u16, project[project_len], version[version_len]
```

The device accepts only the ESP-IDF-selected non-running app partition and
rejects oversized images. BEGIN_RESPONSE returns
`job_id:u32, total_size:u32, chunk_max:u16, label_len:u8, label[]`. DATA is
`offset:u32, bytes[]` and must be strictly sequential. Every chunk updates a
PSA SHA-256 operation and `esp_ota_write`. END requires exact byte count,
full-image SHA match, a valid ESP-IDF image, exact agreement with the
project/version metadata supplied in BEGIN, and a successful recovery adapter
before selecting the boot partition. When
`CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` is enabled, END additionally
requires the image project name to equal the running firmware project name.
The Gateway honors the advertised capability before recovery entry or direct
OTA. The option defaults off. The default weak recovery hook returns
`ESP_ERR_NOT_SUPPORTED`, which deliberately rejects boot-slot selection.
CANCEL, job cancellation, disconnect or any error calls `esp_ota_abort`.

STATUS has an empty request and returns
`job_id:u32, total_size:u32, received:u32, progress_permille:u16, active:u8,
label_len:u8, result:i32, label[]`. A host may use it after a response timeout
to determine whether the last chunk was committed and resume at the exact
reported offset.

`esp_iris_platform_select_ota_target()` lets a recovery image avoid the
retained last-known-good slot. `esp_iris_platform_prepare_ota()` records
last-known-good/target metadata without teaching Iris a custom partition
layout. `esp_iris_mark_healthy()` remains gated by product acceptance. RESTART
records planned intent, drains its response, then reboots. Factory, NVS,
coredump and crash-evidence partitions are never OTA targets. Crash collection
is an independent evidence workflow; the Gateway does not infer that a crash
was caused by an OTA operation.

## Compatibility vectors

[`golden_vectors.json`](golden_vectors.json) is the normative byte-level v1
compatibility set. Device C and PC Python codec tests consume the same file.
Any intentional envelope change requires a new negotiated protocol version;
existing v1 vectors are never rewritten to reinterpret an existing field.
