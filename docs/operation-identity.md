# Operation request identity (IRIS-P05)

`X-Operation-ID` is bound durably to the device ID, action, typed canonical
parameters, actor kind/name and sorted scopes. Binary parameters contribute
their byte length and SHA-256, never their raw contents. Object key order does
not affect identity. System Update binds the archive SHA-256, not the generated
local evidence filename. A persisted versioned fingerprint makes retries
independent of a Gateway restart.

The same ID and request returns the existing record without repeating the
device call. A different request returns HTTP 409 `operation_id_conflict`.
SQLite's primary-key conflict handling also enforces this across independent
connections. Schema version 5 adds a nullable fingerprint: older records stay
readable but are not assigned fabricated fingerprints. Reusing a legacy ID is
rejected; inspect its outcome before deciding whether to issue a new operation.

This is request deduplication within retained Gateway history, not exactly-once
execution across loss of the database or an uncertain device write.

Regression coverage: `test_operation_identity.py` covers synchronous and
background submission, actor boundaries, changed artifacts, reordered keys,
parallel registration, database reopen/migration and HTTP conflict responses.
`test_gateway_v1.py` checks repeated System Update archive requests.


## HTTP caller contract audit

Every operation call also supplies the common device and actor/scopes boundary.
The endpoint-specific identity includes the actual adapter inputs:

| Operation | Bound request data |
| --- | --- |
| `rpc.raw` | Service/method IDs, deadline, decoded request length and SHA-256 |
| `rpc.<catalog-name>` | Catalog name, resolved numeric IDs, deadline, encoded JSON length and SHA-256 |
| `console.execute` | Resolved numeric IDs, deadline, complete normalized command length and SHA-256 |
| `job.query`, `job.cancel` | Action and job ID |
| `device.restart` | Delay |
| `recovery.enter_factory` | Fixed factory-Recovery target |
| `media.screenshot` | Complete description and save flag |
| `media.mirror_start`, `media.mirror_stop` | Action, channel, FPS, and start description |
| `input.gesture` | Complete event object, including every move and auxiliary field |
| `audio.upload` | Content type, length and content SHA-256 |
| `firmware.ota` | Archived image metadata with BIN/ELF hashes, artifact ID, execution/validation modes and compatibility contract |
| `firmware.system_update` | Bundle manifest/component hashes, archive SHA-256 and compatibility contract |
| `file.upload` | Volume/path, length, actual content SHA-256, overwrite flag and If-Match |
| `file.mkdir`, `file.delete` | Volume and path |
| `file.rename` | Volume, source and destination |

File uploads are fully received into a temporary disk spool (maximum 32 MiB)
and hashed before registering the operation or calling the device. Thus
same-length uploads with different content cannot reuse a prior operation. The
spool is streamed to the existing file adapter and closed on success, conflict,
cancellation or failure. This trades immediate device streaming for a verified
content identity without retaining the full upload in Python memory.

Duplicate screenshots return the existing operation and `idempotent_reuse`
JSON, with `screenshot: null`, because only a summary of the binary result is
retained. They do not re-capture the device or misinterpret that summary as a
new screenshot. A different description with the same ID conflicts.

`test_http_operation_payload_identity.py` verifies equal-length changed RPC,
console, input, mirror, audio and file payloads, screenshot reuse, and identical
raw RPC bytes expressed as text/hex/base64. Each reuse calls the adapter once;
changed content returns 409 without changing the original record.
