# ESP-Iris M6-M9 USB service board test

This internal 2 MB application is an application-only flashing fixture for the
Mosaico High-Speed CDC0 link. It registers deterministic service data:

- binary RPC service 1, method 1 echoes its request body;
- a pull screenshot backend returns a 2×2 RGB565 image;
- IMAGE streaming submits a four-byte synthetic JPEG while enabled.

It uses the same factory-only partition and MMU profile as
`example/esp_iris_minimal`, so it can be temporarily written with `app-flash`
without changing the partition table, NVS or retained evidence. Restore the
minimal/product application after the service test.
