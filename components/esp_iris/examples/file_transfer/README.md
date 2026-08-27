# Streamed file transfer

This USB CDC0 example mounts the `storage` flash partition as a wear-levelled
FATFS volume and exposes only its `/files` mount through ESP-Iris. The logical
volume ID visible to the Gateway is `files`.

The example enables streamed upload and download, directory listing and
creation, rename, and safe deletion. It creates `README.txt` on the first boot
so a download is available immediately. Files persist across resets. If the
partition cannot be mounted, this example formats it; flashing a new partition
table can therefore erase previous example files.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/file_transfer \
  -B build-file-transfer build

idf.py -C components/esp_iris/examples/file_transfer \
  -B build-file-transfer \
  -p /dev/serial/by-id/<programming-port> flash
```

Application CDC0 is the ESP-Iris binary endpoint. Use a separate UART or USB
Serial/JTAG programming interface for flashing and monitoring.

## Exercise the example

Start the Gateway and open `http://127.0.0.1:8443/`:

```bash
python components/esp_iris/tools/esp_iris.py web
```

Select the device and open **Files**. The `files` volume should contain
`README.txt`. Download it, upload a new file, create a directory, rename an
entry, and delete a file or an empty directory to exercise the service.

The FATFS backend used here does not advertise
`ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE`, so creating new files is supported but
overwriting an existing path is rejected. Delete or rename the old file first,
or use a backing VFS with verified atomic-replace semantics in your product.

Return to the [example index](../README.md).
