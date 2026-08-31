# ESP-Iris services HIL fixture contract

This contract is private to `test_apps/services_usb`. It does not extend the
public Iris v1 protocol. All integers are little-endian and all RPCs use
service `0x7ffe` unless stated otherwise.

| Method | Name | Request | Response |
| --- | --- | --- | --- |
| `1` | `STATE` | empty | `StateV1` below |
| `2` | `LOG_BURST` | `<HHHH>` stdout records, stderr records, bytes/record, flags | empty |
| `3` | `LIFECYCLE_CYCLE` | empty | empty; after draining the response the fixture stops, unregisters and re-registers RPC/screen services, then starts |
| `4` | `MEDIA_CONFIGURE` | `<BHB4x>` channel, format, period in ms | empty |
| `5` | `STOP_FOR_FLASH` | empty | empty; Iris stops after the response drains |
| `6` | `EXERCISE_BOUNDARY` | empty | `<8i>` invalid RPC, duplicate RPC, RPC table full, invalid Job, Job table full, volume table full, invalid media channel, oversized media |

`StateV1` is `<HH21I12s>`:

1. schema (`1`) and `esp_iris_transport_kind_t`;
2. started, lifecycle, start/stop/register/unregister counts;
3. stdout records, stderr records and exact attempted log bytes;
4. submitted IMAGE frames, AUDIO frames and media errors;
5. Jobs created, finished and cancelled;
6. last `esp_err_t`, pointer count, invalid frames, dropped log bytes,
   minimum Iris stack bytes and Iris internal heap bytes;
7. the last normalized 12-byte pointer RPC message.

Additional fixture RPCs are:

| Service/method | Behavior |
| --- | --- |
| `1/1` | binary echo, including an empty or maximum-size body |
| `1/2` | start Job; optional `<BBH>` behavior (`0` success, `1` failure), reserved byte, duration ms; returns `<I>` job ID |
| `1/3` | delayed echo; `<H>` delay ms followed by the body |
| `1/7` | return the requested `<i>` `esp_err_t` |
| `0x1001/1` | fixed pointer message `<BBhhHI>`; x/y are clipped to `0..479`, stored, and echoed |

The fixture exposes `fs` (FAT read/write), `ro` (read-only view), and
`atomic` (LittleFS atomic-replace) volumes. LittleFS is pinned by the fixture
manifest to `joltwallet/littlefs==1.22.3`.
