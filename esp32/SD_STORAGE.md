# Hoshino ESP32 SD Storage Service

This adds a read-only SD-card service to the existing Hoshino ESP32 watch gateway without changing the current SPP authentication flow.

## Hardware wiring (classic ESP32 / ESP32-WROOM)

Use a microSD breakout that is safe with **3.3 V logic**.

| microSD | ESP32 |
|---|---|
| VCC | 3.3V (or the module's documented supply) |
| GND | GND |
| SCK/CLK | GPIO18 |
| MISO/DO | GPIO19 |
| MOSI/DI | GPIO23 |
| CS | GPIO5 |

GPIO16/GPIO17 remain reserved for the existing Hoshino setup trigger and are not touched.

The SD card is mounted lazily. Bluetooth authentication therefore runs with the same startup memory headroom as before; the SD stack is only initialized after a storage API is actually requested.

## Card layout

Only `/hoshino` on the card is exposed by the API. Requests containing traversal segments such as `..` are rejected.

Example:

```text
/hoshino/
  watch/
    config.json
  faces/
    hoshino.bin
  images/
    cover.png
  games/
    demo.bin
```

## HTTP API

All storage endpoints require the same configured `X-Hoshino-Token` used by the existing LAN management API.

- `GET /api/v1/storage/status`
  - Lazily mounts the SD card and returns capacity/usage and configured SPI pins.
- `GET /api/v1/storage/list?path=faces`
  - Lists at most 48 entries below `/hoshino/faces`.
- `GET /api/v1/storage/file?path=faces/hoshino.bin`
  - Streams the raw file without loading the entire file into ESP32 RAM.
- `GET /api/v1/storage/chunk?path=faces/hoshino.bin&offset=0&length=1024`
  - Returns a JSON object containing a Base64 block. `length` is limited to 2048 bytes.
  - This endpoint is intended for Vela QuickApp code because it works over the already-proven text/JSON `@system.fetch` path.

The response from `/chunk` looks like:

```json
{
  "ok": true,
  "encoding": "base64",
  "offset": 0,
  "bytes": 1024,
  "nextOffset": 1024,
  "size": 12345,
  "eof": false,
  "data": "..."
}
```

## QuickApp client

`Hoshino_QuickApp_Source/src/common/esp32_bridge.js` now exports:

- `storageStatus(callbacks)`
- `storageList(path, callbacks)`
- `storageChunk(path, offset, length, callbacks)`

These use the bridge's existing base URL and management token.

## Android/Termux mock

`tools/android_storage_mock.py` mirrors the same read-only endpoints. It can be used to validate the QuickApp/API path before a physical SD module is connected.

Safe loopback-only test mode:

```sh
python android_storage_mock.py --host 127.0.0.1 --port 8088 --root /sdcard/HoshinoBridge/storage --token hoshino-local
```

For an intentional LAN test, change `--host` to `0.0.0.0` and use a non-default token.

## Design limits

- SD expands persistent storage, not ESP32 RAM.
- Raw-file streaming is preferred for large transfers when the client supports binary responses.
- Base64 chunks trade bandwidth for compatibility; 1024-byte chunks are a reasonable default on the watch.
- Do not power a bare microSD card with 5 V logic.
