# ESP-Iris M6-M9 USB service board test

This internal 2 MB application is an application-only flashing fixture for the
Mosaico High-Speed CDC0 link. It registers deterministic service data:

- binary RPC service 1, method 1 echoes its request body;
- a pull screenshot backend returns a 2×2 RGB565 image;
- IMAGE streaming submits a four-byte synthetic JPEG while enabled;
- a wear-levelled FATFS partition is exported as the `fs` logical volume for
  upload, download, directory creation, rename and delete tests.

The fixture uses a custom 2 MB partition table and must be written with a full
`flash`, not `app-flash`. The `storage` partition can be formatted when first
mounted. FATFS does not provide atomic replacement semantics, so the fixture
intentionally does not advertise `ATOMIC_REPLACE`; create-only uploads are
still fully supported. Restore the minimal/product application and its
partition table after the service test.

With a source Gateway connected to the fixture, run the reusable hardware
matrix with:

```bash
python hardware_file_e2e.py --gateway http://127.0.0.1:8879
```

The matrix transfers a deterministic 13,337-byte file, verifies its SHA-256
and an unaligned HTTP Range read, then covers mkdir, rename, non-empty directory
protection, unsupported atomic-overwrite reporting, path traversal rejection,
operation audit records and cleanup.
