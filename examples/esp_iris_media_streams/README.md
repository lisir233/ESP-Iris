# ESP-Iris media streams

This USB example publishes two bounded synthetic streams without a display,
camera, microphone, BSP, or GSP dependency:

- IMAGE: a valid 40 x 24 RGB565 animation (1,920 bytes per frame).
- AUDIO: 16 kHz mono PCM S16LE, emitted as 100 ms chunks containing a 500 Hz
  test tone (3,200 bytes per chunk).

Both producers call `esp_iris_media_is_streaming()` before generating data.
They remain idle until the PC starts the corresponding mirror channel and
automatically become idle again after mirror stop or disconnect.

## Build and run

From an initialized ESP-IDF 5.5 or newer shell:

```bash
idf.py build
idf.py -p /dev/serial/by-id/<device> flash
```

Flash over the board's Serial/JTAG or UART interface. ESP-Iris exclusively
owns the separate high-speed TinyUSB CDC0 interface; it is a binary protocol
endpoint, not a text console.

In the Workbench, start the image stream to see the moving RGB pattern or
start audio recording to capture the synthetic PCM tone. The generic media
descriptor represents audio as `width=16000` (sample rate), `height=1`
(channels), `stride=2` (bytes per sample frame), and
`format=ESP_IRIS_AUDIO_FORMAT_PCM_S16LE`.
