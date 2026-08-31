# Bounded image and audio streams

This USB CDC0 example publishes two synthetic streams without a display,
camera, microphone, BSP, or GSP:

- **IMAGE:** RGB565 by default, with RGB888, valid embedded JPEG, and valid
  embedded PNG build profiles.
- **AUDIO:** 16 kHz mono PCM S16LE, emitted as 100 ms chunks containing a
  500 Hz test tone, 3,200 bytes per chunk.

Both producers check `esp_iris_media_is_streaming()` before generating data.
They remain idle until the host starts the corresponding channel and return to
idle after stop or disconnect.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/media_streams \
  -B build-media-streams build

idf.py -C components/esp_iris/examples/media_streams \
  -B build-media-streams \
  -p /dev/serial/by-id/<programming-port> flash
```

Select another image format by adding an overlay defaults file while keeping
the base transport configuration:

```bash
idf.py -C components/esp_iris/examples/media_streams \
  -B build-media-jpeg \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.jpeg.defaults' build
```

The available overlays are `sdkconfig.rgb888.defaults`,
`sdkconfig.jpeg.defaults`, and `sdkconfig.png.defaults`. RGB565 is the default.
JPEG and PNG are static encoded fixtures; the raw formats animate. Opus is not
embedded because a product should submit packets from its own encoder.

Use a separate UART or USB Serial/JTAG programming interface; application CDC0
is reserved for the ESP-Iris binary link.

## Expected result

Start the image stream in the Workbench to see the selected format. Start audio
capture to receive the PCM tone. The audio media descriptor uses:

```text
width=16000   sample rate
height=1      channel count
stride=2      bytes per sample frame
format=ESP_IRIS_AUDIO_FORMAT_PCM_S16LE
```

Stopping either channel must stop its producer without creating a device-side
backlog.

Return to the [example index](../README.md).
