# Changelog

All notable ESP-Iris component changes are documented in this file.

## Unreleased

- Add an opt-in, recovery-only System Update service with signed manifest
  transport, bounded multi-component streaming, product-owned Flash policy,
  an independently registrable read-only Flash inventory, Gateway closed-loop
  validation, fixed-range image padding, and signed `.irisfw` bundle tooling.

- Allow TCP, application USB CDC0, and USB Serial/JTAG to wait concurrently,
  with bounded HELLO_ACK arbitration and one active transport owner.
- Add a standalone FATFS-backed file-transfer example covering streamed
  upload/download, directory operations, rename, and safe deletion.
- Allow cross-project OTA by default and add
  `CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` for products that require
  the target image to retain the running firmware project name.
- Keep Linux USB recovery-first OTA attached across product-string
  re-enumeration by preferring stable physical `by-path` endpoints and treating
  the old session's `ConnectionError` as transient while recovery reconnects.

## 0.1.0

- Initial public component release.
- Add bounded USB CDC0, USB Serial/JTAG and raw TCP transports.
- Add binary logging, status, RPC/jobs, media, pairing, crash evidence and OTA.
- Include the Python Developer Gateway and React workbench sources.
