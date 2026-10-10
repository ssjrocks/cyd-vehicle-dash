# Changelog

All versions were developed on 2026-10-10/11. The firmware version is `FW_VERSION` in `obd_probe/obd_probe.ino`.

## 1.13.0
- **Screen sleep:** backlight off and drawing paused after 60 s with no dongle and no touch. Wakes on touch (the waking touch doesn't press a button) or when the dongle connects.
- **Listen-before-connect:** BLE auto-reconnect does a 3 s passive scan for the saved dongle and only connects when it's advertising. Retries every 10 s on the dash build, and logs only when the result changes.
- **Fixed:** the battery readout showed "f" instead of the voltage (`LV_SPRINTF_USE_FLOAT`).

## 1.12.0 – 1.12.2
- **Replacement-dash layout:** top tabs Dash / Faults / All data / Bridge.
  - Speedometer and tachometer.
  - Small coolant and intake-air gauges with readings and captions outside the gauge.
  - Battery voltage in the bottom status slot.
- **Bridge screen:**
  - WiFi mode: off / home WiFi with hotspot fallback / hotspot only.
  - TCP bridge on/off.
- **WiFi start is non-blocking:** the dash appears immediately.
- Faults screen adds the KWP2000 fault-memory read (`18 00 FF 00`).
- **Fixed:** boot loop caused by setting an LVGL table's row count to 0.

## 1.11.0
- **Waveshare ESP32-S3-Touch-LCD-4.3 support:** first touchscreen UI on LVGL 8.4.
  - RGB panel driven directly from the pins and timings of the board's ESPHome config.
  - GT911 touch via the CH422G expander.
  - LVGL heap in PSRAM.
- **Shared locks:** web, touchscreen, TCP bridge and auto-reconnect no longer interleave commands on the dongle.
- `build.sh` for all three boards.

## 1.10.0
- Auto-reconnect pauses while the web page is in use, and a *Reconnect now* button was added. Classic-mode reconnects starved WiFi of airtime.

## 1.9.0 – 1.9.2
- `/api/prepare-update`: stops Bluetooth before the upload is parsed, so OTA works on the memory-starved Classic build.
- The reconnect task is created once at boot instead of per attempt.
- **Fixed:** a crash from calling the Bluetooth serial library after it was shut down.
- **Fixed:** a crash from a `String` allocation failing under memory pressure.
- Smaller log and receive buffers.

## 1.8.0 – 1.8.2
- **Freeze watchdog:** reboots if the main loop stalls for 150 s, and logs where it stalled on the next boot.
- Reset reason in status.
- Crash summary from the core-dump partition at `/api/crash`.

## 1.7.0 – 1.7.1
- Auto-reconnect runs in a background task, so a sleeping dongle no longer blocks the web server.
- The Classic SPP channel found by SDP is remembered.
- `/api/status` lists recent HTTP clients.

## 1.6.0
- On-board LED lit while running (power-bank keep-alive attempt on the original ESP32 board).

## 1.5.0
- Classic SPP handshake logged step by step (SDP, pairing, RFCOMM), with fallbacks to channel 1 and PINs 0000/6789.
- Found that the test dongle uses RFCOMM channel 2.

## 1.3.0 – 1.4.0
- Original ESP32: only one Bluetooth stack (Classic or BLE) per boot, switching automatically with a reboot.
- OTA stops Bluetooth first.
- Log replies are streamed in chunks.

## 1.0.0 – 1.2.0
- First version on an ESP32-C3:
  - BLE scan and connect with full GATT dump and automatic RX/TX selection.
  - Web UI with terminal, adapter fingerprint, vehicle probe and live data.
  - Raw TCP bridge on port 35000 and the JSON API.
- OTA from the web page and ArduinoOTA.
- First original-ESP32 build with Classic SPP.
- **Fixed:** a log-buffer index bug that made the web log repeat.
