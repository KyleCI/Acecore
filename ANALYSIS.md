> **Firmware 1.0.1 reference.** The collector on this branch is firmware **2.5.0**.
> This file is kept from Dubem's tree. For the current collector (app `/v2/*`, `api.acecore.tech` vs `api-dev.acecore.tech`, factory identity), start at [README.md](README.md).

# IOT Layer — Optimization & Concurrency Analysis

ESP32 collector/gateway firmware (`device_id = ACE2025`). PlatformIO, `env:esp32dev`,
Arduino framework. This document covers what changed, why, and what was deliberately
left alone.

---

## 1. The core problem: blocking work on the main loop and the request path

The original `loop()` ran two blocking operations on **every iteration**, and one of
them also ran on **every web request**:

| Operation | Where it ran | Cost |
|---|---|---|
| `updateADC()` | `loop()` | ~27 ms — 9-sample median × 3 channels, each with `delay(1)` |
| `readOutput()` (PZEM-004T) | `loop()` **and** `getSensorData()` | Blocking Modbus-over-UART; tens of ms, up to a library timeout if the meter is silent |
| `Serial.printf(...)` | `loop()`, unthrottled | One full line every iteration |
| `getBatteryPercent()` | `updateLEDColor()` (every loop) + every web request | A `Serial.printf` per call |

Two consequences:

1. **`getSensorData()` triggered a blocking PZEM read inside the AsyncTCP handler.**
   `/metrics` and `/sensor-data` are served from the AsyncWebServer task. A blocking
   Modbus transaction there stalls the whole server — and the dashboard polls `/data`
   every 500 ms while the upload task calls `getSensorData()` every 500 ms, so the
   meter was being hammered from multiple contexts.

2. **The SPI/MPPT path could not run at all.** `handleSPI()` blocks on
   `spi_slave_transmit(..., portMAX_DELAY)`. Calling it inline would freeze the loop
   until the Arduino MPPT master sent a frame — which is exactly why both
   `// handleSPI();` in `loop()` and the `mppt_data` block in `getSensorData()` were
   commented out. The feature was effectively dead.

---

## 2. What moved to the background

### `sensorTask` — new (`src/sensor_task.cpp`, `include/sensor_task.h`)
Owns **all** local sampling: `updateADC()` + the four derived getters + the PZEM read.
Runs every `SENSOR_SAMPLE_INTERVAL_MS` (default 250 ms), publishes a `SensorCache`
snapshot under a mutex. Everyone else — `loop()`, the LED logic, `getSensorData()`,
the upload task — now reads the cached copy via `getSensorCache()`. **Zero blocking
sensor work on the request path.** Pinned to core 1, priority 2.

### `spiTask` — new (`src/spi_task.cpp`, `include/spi_task.h`)
Wraps `handleSPI()` in a dedicated task. It sleeps inside `spi_slave_transmit` while
idle (≈ no CPU), parses each MPPT frame, and caches the serialized JSON object under a
mutex with a timestamp. Exposed via `getMpptJson(maxAgeMs)` / `mpptDataFresh()`. This
**re-enables the MPPT feature** that was previously impossible to run inline. Pinned to
core 1, priority 1.

### `uploadTask` — unchanged, but now correct
Already a background task. It calls `getSensorData()`, which is now a cheap cached read
instead of a blocking PZEM transaction — so the HTTP task no longer blocks on the meter.

### Core/priority layout
`sensorTask` and `spiTask` are pinned to core 1 (APP_CPU) so their blocking work stays
off core 0, where WiFi and the ESP-NOW receive callback live. The AsyncWebServer runs
on its own task. `loop()` ends with `vTaskDelay(10 ms)` so it yields cleanly.

---

## 3. Correctness fixes made along the way

These are behavioral fixes, called out so you can revert any you disagree with:

1. **`power_drawn` / `energy_consumed` were always `0.0`.** `getSensorData()` emitted
   the globals `outPower` / `outEnergy`, which are defined to `0.0` and never written.
   Now wired to the live PZEM `power` / `energy` from the cached snapshot.
2. **`solar_panel_voltage` was a hardcoded `12.50` shadow.** Now wired to the cached
   `readSolarVoltage()` value.
3. **`updateLEDColor()` had no return on the fall-through path** (control could reach
   the end of a non-`void` function → undefined return). Added an explicit
   `return redLED;`.
4. **`getBatteryPercent()` did a `Serial.printf` on every call** — and it was called
   from the LED update every loop and from every web request. Print removed; logging
   now happens once per second in `loop()` from the cached snapshot.
5. **Duplicate includes / dead `// handleSPI();`** removed from `main.cpp`. (Note: the
   duplicate `/monitor` route still exists — see §5.)
6. **`MPPT_VERBOSE` guard** added in `mppt.h`. The per-frame formatted table + raw dump
   + JSON block now compile out with `-D MPPT_VERBOSE=0`. They run in the background SPI
   task now, so they no longer block anything, but they are extremely chatty at the
   master's frame rate. Default is `1` (current behavior preserved).

---

## 4. New / changed HTTP surface

- **`GET /mppt`** — new, additive. Returns the latest cached MPPT frame as JSON
  (`{}` when none is fresh within 5 s). Non-breaking.
- **`GET /metrics`, `/sensor-data`** — same shape as before, plus a `mppt_data` field
  (the parsed MPPT object when fresh, otherwise `null`). This re-enables the block that
  was commented out. If you don't want MPPT in this payload, drop the `mppt_data` lines
  in `getSensorData()`.

---

## 5. Recommendations I did **not** apply (out of scope / higher risk)

Listed so they're on your radar; none are required for the changes above to work.

1. **`devices[]` is shared without a lock.** It's written from the ESP-NOW receive
   callback (core 0) and read from web handlers / `buildJSON()` (AsyncTCP task). Today
   it mostly works because writes are word-sized, but `String name` assignment and
   multi-field updates are not atomic. A short critical section (or a dedicated
   `deviceMutex`) around the device-table mutation in `onReceive()` and the read in
   `buildJSON()` would make it correct. I left it untouched to avoid changing the
   ESP-NOW hot path.
2. **`uploadTask` always POSTs every 500 ms.** `anyPending` is initialized to `true`
   and never gates anything, so a request fires even with no socket data. If that's
   intentional (always ship the collector's own reading), fine; if not, gate on whether
   any slot or local data actually changed.
3. **Credentials are hardcoded** in `config.h` (`routerSSID`/`routerPASS`, `apiKey`) and
   `provisioning.h` (`AP_PASS`). The NVS provisioning flow already exists — consider
   removing the literals from the source tree before sharing the repo.
4. **`wifi_manager.cpp::connectWiFi()` is dead code** in the current flow — `setup()`
   uses `connectToSaved()` from `provisioning.cpp`. `checkWiFi()` is still used and is
   fine. Worth removing to avoid the two competing WiFi-bring-up paths.
5. **BLE `Status` command calls `analogRead()` directly.** If you ever enable BLE
   (`handleBLE()` is not called today), that races the sensor task on the ADC. Route it
   through `getSensorCache()` instead.
6. **`StaticJsonDocument<768>`** in `mppt.cpp::displayJSONOutput()` is deprecated in
   ArduinoJson v7 (compiles with a warning). The new code uses the v7 `JsonDocument`.
   Migrating the old call removes the warning.

---

## 6. Net effect

- `loop()` no longer blocks: ~27 ms ADC + a blocking PZEM read per iteration → cached
  reads + a 10 ms yield.
- The web server never blocks on the meter; `/data`, `/metrics`, and `/sensor-data`
  return from cache.
- The MPPT/SPI path is alive for the first time, isolated on its own task.
- Serial output dropped from per-iteration to ~1 Hz (plus optional MPPT frames).
- Repo cleaned: `.pio/` build artifacts (~106 MB), `*.rar` archives, and `__MACOSX`
  junk excluded from this deliverable. `.pio/` regenerates on `pio run`.
