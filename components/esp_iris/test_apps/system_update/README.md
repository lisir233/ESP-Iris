# ESP-Iris system-update protocol fixture

This internal fixture enables the complete System Update protocol and
registers a deterministic, non-flashing backend. It is used to compile and
exercise manifest authorization, component streaming, status, cancellation,
commit and inventory reporting without granting raw Flash access.

The fixture deliberately does not model product policy. Real recovery firmware
must explicitly decide whether signatures are required, validate fixed system
regions, stream only the future application directly to Flash, retain
bootloader/partition-table bytes in internal RAM, read back every write, and
persist the final operation result in product-owned system metadata.
