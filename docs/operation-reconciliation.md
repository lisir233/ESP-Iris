# Uncertain write outcomes (IRIS-P06)

A missing reconnect/HEALTHY observation after OTA or System Update is
`outcome_unknown`, not proof that the write failed. Lost transport during these
mutations and interrupted running firmware writes also remain uncertain. An
explicit verification mismatch can still fail acceptance. No automatic write
replay is performed.

The acceptance deadline uses the product HELLO `health_timeout_ms` (legacy
default 45000 ms). `web --ota-health-timeout SECONDS` overrides it for the
Gateway process; values must be finite and within 1–600 seconds. It applies to
both OTA and System Update. Progress retains writer Boot ID across stages so
later observations can distinguish an old boot from an accepted new one.

Use `ctl operation-reconcile OPERATION_ID` or
`POST /v1/operations/{operation_id}/reconcile` to observe an uncertain operation.
This only queries live STATUS and, for System Update, inventory. Each RPC is
bounded to ten seconds. It never invokes OTA, restart, recovery, file writes or
raw RPC. Reconciliation remains available in observe mode. The original
operation and terminal status are immutable; a new schema-version-6 record
links to it with one of these outcomes:

- `observed_success`: OTA requires the expected project and ELF hash, explicit
  normal firmware role, a changed Boot ID and a retained HEALTHY event for that
  live boot after the original request. System Update requires a matching
  operation receipt, successful commit result, target layout, normal role,
  changed boot and HEALTHY evidence. A targeted bootloader must match its
  component SHA-256. A targeted application must match its project and ELF hash,
  captured before writing and bound to the request component BIN SHA-256;
  legacy records without this identity remain unknown. A second STATUS detects
  a reboot during inventory collection. Normal OTA acceptance also checks that
  the final STATUS Boot ID is the same boot that emitted HEALTHY.
- `observed_failure`: a System Update receipt for this exact operation reports
  a failed commit.
- `outcome_unknown`: identity, healthy evidence, receipt or connectivity is
  insufficient; unsupported actions also remain unknown.

These are observations at a point in time, not claims of exactly-once device
execution. An OTA matching only a version string cannot be reconciled to
success. Missing or expired HEALTHY history stays unknown. A later unrelated
write may supersede a receipt; that does not prove the earlier write failed.

`GET /v1/operations/{operation_id}/reconciliations` retrieves the append-only
records, which survive Gateway restart and appear in exported
`reconciliations.jsonl`. Existing authentication rules apply. Tests cover
missing HEALTHY, interrupted writes, policy bounds, offline HTTP observations,
matching/mismatching firmware and receipts, unchanged original history and
export persistence. No hardware result is implied by these host tests.
