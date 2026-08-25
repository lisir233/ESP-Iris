# Bounded image and audio streams

This USB CDC0 example publishes two synthetic streams without a display,
camera, microphone, BSP, or GSP:

- **IMAGE:** a 40 × 24 RGB565 animation, 1,920 bytes per frame.
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

Use a separate UART or USB Serial/JTAG programming interface; application CDC0
is reserved for the ESP-Iris binary link.

## Expected result

Start the image stream in the Workbench to see the moving RGB pattern. Start
audio capture to receive the PCM tone. The audio media descriptor uses:

```text
width=16000   sample rate
height=1      channel count
stride=2      bytes per sample frame
format=ESP_IRIS_AUDIO_FORMAT_PCM_S16LE
```

Stopping either channel must stop its producer without creating a device-side
backlog.

Return to the [example index](../README.md).
