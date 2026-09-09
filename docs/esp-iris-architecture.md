# ESP-Iris architecture and engineering contract

ESP-Iris keeps device work bounded and moves durable control, observation and
presentation to the PC. Dependencies flow downward only:

```text
React Workbench / source CLI
          -> HTTP + WebSocket adapter
          -> Gateway application service
          -> GatewayHub protocol
          -> IrisHub -> DeviceSession -> Link (USB/TCP)
          == frozen ESP-Iris v1 wire contract ==
          -> device transport -> runtime/session coordinator
          -> RPC/jobs, media, OTA/auth services
          -> product callbacks and recovery adapter
```

## Ownership and dependency rules

| Layer | Owns | Must not own |
|---|---|---|
| Frontend/CLI | Presentation and `/v1` clients | USB, protocol frames, SQLite |
| HTTP adapter | Authentication middleware, validation, response mapping | Link/session state |
| Gateway application | Operation workflows, mode and evidence policy | Serial/TCP implementation |
| Hub | Discovery, reconnect and multi-device fan-out | HTTP or database schema |
| Session | Wire handshake, request correlation, credit and time sync | Device discovery or UI policy |
| Link | Bounded bytes over USB/TCP | Protocol semantics |
| Device runtime | Lifecycle/session scheduling and prioritization | Product acceptance policy |
| Device services | RPC/jobs, media, authentication and OTA mechanics | Product partition policy |
| Product adapter | Acceptance, recovery metadata and product RPCs | Gateway/HTTP behavior |

## System image mutation boundary

System Inventory and System Update are deliberately separate registrations.
Normal firmware may register only the read-only inventory provider, which
reports hashes calculated from current protected Flash ranges and the last
fixed-address sysmeta result. Recovery firmware registers the same provider
plus the write backend. ESP-Iris advertises capabilities from those runtime
registrations rather than merely from Kconfig.

The generic device service owns bounded framing, sequential offsets, streaming
SHA-256, cancellation, status publication and retained-job integration. It
does not parse a target offset as write authority and contains no raw-Flash
endpoint. The product backend is the Flash-policy boundary: it validates image
formats, protected ranges and descriptors, and defines staging and commit
policy. Signature verification and pinned release keys are product capabilities,
not guarantees of the generic interface. The current ESP-Mosaico backend accepts
unsigned plans and stages protected images in PSRAM. Authenticated manifests,
key provisioning and hardened network deployment remain planned security work;
the current implementation must not be described as providing those guarantees.

The PC-side release builder creates an `.irisfw` with a canonical manifest and
fixed-range `0xff` padding; authentication depends on the selected product policy.
The Gateway checks the source layout, enters retained
recovery, streams the plan, and accepts success only after a new healthy
normal boot reports matching actual inventory, operation ID and application
identity. Workbench and CLI are clients of that same `/v1` operation; neither
has a device-protocol or Flash bypass.

The Gateway application depends on `GatewayHub`, not concrete `IrisHub` or
`DemoHub` internals. Device lifecycle/session transitions and Gateway
operation/session transitions are explicit state machines. Terminal operation
states are immutable; a caller must use the original operation ID to observe a
write whose outcome is not yet established.

## Contracts

- `protocol/spec.md` and `protocol/golden_vectors.json` are the device/PC wire
  contract. Existing v1 vectors are immutable.
- `openapi_contract.py` is the executable HTTP contract.
- `rpc_catalog.json` is the named product RPC contract.
- Events use `esp-iris-event/v1`; metrics use `esp-iris-metrics/v1`.
- SQLite uses ordered migrations and rejects a database newer than the running
  Gateway.

Any contract change requires a compatibility test. Additive fields must remain
skippable. Reinterpreting an existing field requires a new negotiated version.
The hardware-derived Device ID rollout and legacy-host behavior are documented
in [device-identity-migration.md](device-identity-migration.md).

## Test boundaries

Each module has isolated tests for its state and error behavior. The complete
test pyramid is:

1. Host unit tests for C state/codec and Python/TypeScript modules.
2. Cross-language golden-vector and HTTP/RPC contract tests.
3. Fake-link/Gateway/SQLite/Workbench integration tests.
4. ESP32-S31 build tests and Gateway-only HIL validation.

Fault tests cover malformed/fragmented frames, invalid state transitions,
disconnects, interrupted operations and legacy/future database schemas. HIL
must use `esp_iris.py ctl --json`; it must not open the device USB session
directly.

## Observability

Events correlate, when applicable, by `device_id`, `boot_id`, `session_id`,
`operation_id`, `request_id`, `job_id`, `event_id` and firmware SHA. The
Gateway `/v1/metrics` endpoint exports counters, gauges and duration
distributions; `/v1/health` reports readiness and database schema version.

Device STATUS remains the authority for bounded-device resource signals:
invalid frames, dropped logs, link count, minimum stack headroom, maximum
worker active time and internal heap use. Operation progress is the authority
for OTA/recovery stage and duration. Crash evidence is independent from an OTA
record unless direct evidence connects them.

## Resource budgets

`components/esp_iris/resource_budgets.json` contains reviewable source,
firmware and frontend limits. `tools/check_esp_iris_budgets.py` is run locally
and in CI. Raising a limit requires an explanation in the change; generated
build artifacts are required in the firmware CI job.

## Change protocol

1. Update the relevant contract or architecture decision first.
2. Add or change module tests and fault cases.
3. Run the commands in `.gitlab-ci.yml` and the host matrix in
   `.github/workflows/host.yml`; neither a declared CI job nor an older HIL report
   proves that the current revision passed. Run budget groups separately after
   their artifacts exist; missing requested artifacts fail the check.
4. Preserve BIN, ELF, map, sdkconfig and firmware hashes for HIL.
5. For device validation, expose the Gateway Web workbench to the developer and
   retain the same operation/event evidence used by the Agent.

`tools/release_evidence.py --build <directory> --idf-path <idf> --output <json>`
requires BIN, ELF, map and sdkconfig for each selected profile, records SHA-256
and the Iris/IDF revisions, and reports a dirty checkout explicitly. It does not
claim hardware validation. For ESP-Mosaico, run device validation through the
product `mosaico.py` commands. Do not use the raw-flash fixture runner on a product
device; controlled power-loss tests need a dedicated fixture and retained
identity/partition policy. Additional SoC profiles are planned (IRIS-A01), not
implicitly supported by the ESP32-S31 build matrix.
