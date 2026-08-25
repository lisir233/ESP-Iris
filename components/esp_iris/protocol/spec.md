# ESP-Iris protocol v1

Status: milestones M1-M11 are implemented. The v1 wire envelope is frozen;
newer services use the existing request ID, stream ID, credit and channel
fields.

## Link

Normal firmware compiles exactly one link: USB CDC0 or a raw TCP server. CDC0
contains only ESP-Iris binary frames. Baud rate, line coding and RTS have no
protocol meaning. USB DTR open/close delimits a PC link session, but never
requests reset or ROM download mode.

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
`AUDIO=5`, `OTA=6`, and `CRASH=7`. Unknown types on a known channel produce a
CONTROL ERROR when a response is possible. A future protocol version must use
capability negotiation rather than silently reinterpreting an existing type.

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
PREVIOUS_BOOT_CRASH, optional CORE_DUMP_AVAILABLE, optional SERVICES_READY and
optional HEALTHY. `esp_iris_mark_services_ready()` and
`esp_iris_mark_healthy()` update replayable lifecycle state. A planned
restart event records local intent; products with crash-loop recovery override
`esp_iris_platform_mark_planned_restart()` to persist that intent.

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
full-image SHA match, a valid ESP-IDF image, optional exact project/version
policy, and a successful recovery adapter before selecting the boot
partition. The default weak recovery hook returns `ESP_ERR_NOT_SUPPORTED`,
which deliberately rejects boot-slot selection. CANCEL, job cancellation,
disconnect or any error calls `esp_ota_abort`.

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
