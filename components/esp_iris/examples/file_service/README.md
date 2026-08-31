# Bounded file service

This USB CDC0 example mounts a wear-levelled FAT filesystem and exports it as
the logical volume `fs`. It enables read, list, mtime, write/hash, mkdir,
delete, and rename. It deliberately does not advertise atomic replacement,
because the example does not assume that property from the FAT VFS.

The custom layout requires a 2 MB or larger flash device. A full flash may
format the `storage` partition; verify the target before flashing.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/file_service \
  -B build-file-service build
idf.py -C components/esp_iris/examples/file_service \
  -B build-file-service -p /dev/serial/by-id/<programming-port> flash
```

Application CDC0 is the Iris link. Use a separate UART or USB Serial/JTAG
interface for flashing and monitoring, then start the Gateway.

## Exercise the volume

The Workbench file browser exposes every operation below. The same API can be
driven with `curl` after replacing `DEVICE_ID`:

```bash
BASE=http://127.0.0.1:8443/v1/devices/DEVICE_ID
curl "$BASE/files/volumes"
curl "$BASE/files?volume=fs&path="
curl "$BASE/files/stat?volume=fs&path=README.txt"
curl -o README.txt "$BASE/file?volume=fs&path=README.txt"
curl -H 'Range: bytes=0-15' -o README.prefix \
  "$BASE/file?volume=fs&path=README.txt"
curl -X POST -H 'Content-Type: application/json' \
  -d '{"volume":"fs","path":"demo"}' "$BASE/directories"
curl -X PUT --data-binary @README.txt \
  "$BASE/file?volume=fs&path=demo/upload.txt"
sha256sum README.txt
curl -X POST -H 'Content-Type: application/json' \
  -d '{"volume":"fs","source":"demo/upload.txt","destination":"demo/moved.txt"}' \
  "$BASE/file-rename"
curl -X DELETE "$BASE/file?volume=fs&path=demo/moved.txt"
curl -X DELETE "$BASE/file?volume=fs&path=demo"
```

The upload response contains `sha256`; compare it with the local `sha256sum`
output. Uploads are streamed with strict offsets and SHA-256 verification.
Replacement of an existing file is rejected because this volume omits
`ATOMIC_REPLACE`.

Return to the [example index](../README.md).
