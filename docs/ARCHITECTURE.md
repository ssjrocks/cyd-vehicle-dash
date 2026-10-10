# Architecture

The firmware is one Arduino sketch in `obd_probe/`:

| File | Contents |
|---|---|
| `obd_probe.ino` | Bluetooth (BLE via the core's BLE library, which uses NimBLE on S3/C3 and Bluedroid on the original ESP32; Classic SPP via BluetoothSerial), the ELM327 command engine, WiFi, web server and JSON API, TCP bridge, OTA, logging and diagnostics |
| `ui.h` | The web page (HTML/JS in flash). All OBD decoding for the web UI happens in the browser. |
| `lcd_ui.h` | Touchscreen UI, only compiled for the ESP32-S3 (`HAS_LCD`): panel, touch and IO-expander drivers, the dash, and its own OBD decoding |
| `lv_conf.h` | LVGL 8.4 configuration |
| `secrets.h` | WiFi and hotspot credentials (git-ignored; template in `secrets.example.h`) |

## Tasks

| Task | Does |
|---|---|
| `loopTask` (Arduino loop) | HTTP server, TCP bridge, ArduinoOTA, WiFi fallback, freeze-watchdog heartbeat. HTTP handlers run here and may block for one dongle command. |
| `autoconn` | Created once at boot and woken per attempt. Reconnects to the saved dongle. BLE: 3 s passive scan first, then connect only if the dongle is advertising. |
| `freezewd` | Reboots if `loopTask` hasn't run for 150 s; the stuck stage is kept in RTC memory and logged on the next boot. |
| `lvgl` (S3) | Runs `lv_timer_handler`. Handles screen sleep: while asleep it skips LVGL and only polls the touch controller and the link state. |
| `lcdwork` (S3) | All dongle I/O for the touchscreen: adapter init, PID discovery, dash and all-data polling, fault reads, scan/connect jobs from the UI. |
| Bluetooth host task | BLE notifications, appended to the receive buffer. |

## Locks

- **`opMux`** (recursive): exclusive use of the dongle. Taken around each command by HTTP handlers, the touchscreen worker and auto-reconnect. A connected TCP bridge client holds it for the whole session. Auto-reconnect only tries if it can take it immediately.
- **`connMux`** (recursive): guards the dongle description strings (address, name, GATT JSON, last error, layout). Only connect and disconnect write them. Readers on the status path try it briefly and fall back to placeholders.
- **`lvMux`** (recursive, S3): held while LVGL runs. The worker takes it only to update widgets.
- **Lock order:** `opMux` → `connMux`. `lvMux` is never held while waiting for the other two.
- `logMux` and `rxMux` protect the log ring and the receive buffer.

## Command engine

`runCmd()` clears the receive buffer, writes `cmd + "\r"` to the dongle (BLE writes chunked to the negotiated MTU), and waits for the ELM327 `>` prompt or a timeout.

The touchscreen appends `1` to mode-01 requests when the adapter accepts it (ELM327 "one reply expected"), which saves the reply timeout on each request.

## Saved settings (NVS namespace `obdprobe`)

| Key | Meaning |
|---|---|
| `type`, `addr`, `atype`, `name` | Saved dongle: `ble`/`spp`, address, BLE address type, display name |
| `x1`, `x2` | BLE RX/TX UUID override, or the SPP PIN |
| `ch` | SPP RFCOMM channel found by SDP |
| `btmode`, `btonce` | Original ESP32: Bluetooth stack for this boot, and a one-shot override when a scan or connect needs the other one |
| `auto` | Auto-reconnect on/off |
| `wifimode` | 0 off, 1 home WiFi with hotspot fallback, 2 hotspot only (applied at boot) |
| `bridge` | TCP bridge on/off |
| `rot` | Screen rotated 180° |

## Memory notes

- **Original ESP32:** with Classic SPP plus WiFi, only ~10–18 KB of internal heap is free and the largest block is ~10 KB. Hence: one Bluetooth stack per boot, a streamed log endpoint, small buffers, and Bluetooth stopped before OTA. PSRAM does not help, because the precompiled core keeps WiFi and Bluetooth buffers in internal RAM.
- **ESP32-S3 (dash):** ~60 KB internal heap free with BLE, WiFi and the UI running. Frame buffer, LVGL heap and draw buffers are in PSRAM; the panel's DMA bounce buffers are in internal RAM.
