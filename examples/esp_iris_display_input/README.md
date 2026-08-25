# ESP-Iris display and pointer-input example

This USB example provides a complete pull-style screen backend and the exact
pointer RPC expected by the Developer Gateway. It depends only on ESP-IDF and
the local `components/esp_iris` component; no display driver, BSP, GSP, or
Wi-Fi stack is required.

The backend generates a deterministic 480 x 480 RGB565 color pattern on
demand. It requires no framebuffer and supports both:

- one-shot SCREEN capture through the Gateway screenshot endpoint; and
- SCREEN mirror streaming, generated in bounded scanline chunks.

Gateway pointer input calls service `0x1001`, method `1`. Each message is the
fixed 12-byte little-endian structure `<BBhhHI>` (phase, reserved, x, y,
reserved, sequence). The example validates the exact size and echoes the
message, which lets the Gateway verify delivery without requiring a touch
controller.

Build from an initialized ESP-IDF shell:

```bash
idf.py -C examples/esp_iris_display_input -B build build
```

Flash through the board's Serial-JTAG/UART programming port. Application USB
CDC0 is the Iris binary link, not a serial console. Start the Gateway against
that CDC device, save a screenshot, start a SCREEN mirror at 1-60 FPS, and use
the Workbench pointer gesture control. The resulting screenshot should be a
red/green gradient with a blue XOR pattern.

For a product display, keep the same backend contract but replace
`synthetic_pixel()` with a bounded read from the product framebuffer (and use
locking appropriate to that display stack).
