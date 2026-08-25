# Pull screenshot, screen mirror, and pointer input

This USB CDC0 example implements the screen and pointer contracts without a
display driver, BSP, GSP, or framebuffer allocation.

The screen backend generates a deterministic 480 × 480 RGB565 pattern on
demand. The same bounded reader supports:

- one-shot screenshot capture; and
- continuous screen mirror tiles.

Pointer input uses RPC service `0x1001`, method `1`. The fixed 12-byte
little-endian payload is `<BBhhHI>`: phase, reserved byte, x, y, reserved word,
and sequence. The example validates and echoes the message so delivery can be
tested without a touch controller.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/display_input \
  -B build-display-input build

idf.py -C components/esp_iris/examples/display_input \
  -B build-display-input \
  -p /dev/serial/by-id/<programming-port> flash
```

Application CDC0 is the ESP-Iris binary endpoint; use a separate programming
interface.

## Expected result

In the Workbench:

1. Capture a screenshot and verify the red/green gradient with a blue XOR
   pattern.
2. Start screen mirror at 1–60 FPS and confirm incremental updates.
3. Use the pointer gesture control and confirm that the echoed sequence is
   accepted.

For a product, replace the synthetic reader with a bounded, correctly locked
read from the display stack while keeping the same backend contract.

Return to the [example index](../README.md).
