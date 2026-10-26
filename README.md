# Acecore collector firmware 2.5.0

This branch replaces Dubem's 1.0.1 collector with the v17 line (firmware **2.5.0**).
v15 is the factory identity (`pc_ident`: serial, device secret, label hotspot password).
v16 is HQ `payment_lock`. v17 folds the old NVS `dev_secret` into that same `device_secret`
and drops `SET dev_secret`.

Build from this directory with PlatformIO (`espressif32@7.0.1`):

```bash
pio run                   # default env esp32dev — live cloud api-dev (what Upload hits)
pio run -e esp32dev_prod  # production api.acecore.tech, only when you mean it
pio run -e esp32dev_dev   # same live cloud, bare board (battery ADC off)
make -C test/host         # host tests for lib/pc_core
```

Caleb 2026-10-26: **api-dev.acecore.tech is the live Upload cloud.** `pio run` and the
PlatformIO Upload button use `default_envs = esp32dev`: TLS
`wss://api-dev.acecore.tech/ws/device` and OTA base `https://api-dev.acecore.tech`.
Production `api.acecore.tech` is env `esp32dev_prod` only. Every env keeps the OTA
hardware model string `esp32dev` (`POWERCELL_HARDWARE_MODEL`) so existing units,
including ACE9678, still match the backend model. Firmware version on every env is **2.5.0**.

## Flash rules

After factory identity is in NVS (serial, device secret, label hotspot password):

- Never erase flash. Do not run `pio run -t erase`, esptool `erase_flash`, or an IDE erase-before-upload option.
- Never flash a merged binary (bootloader + partition table + app). Use app-only upload: `pio run -t upload`. A merged image can wipe NVS and the identity with it.

A merged image is only for a blank chip that does not yet have an identity, and only if that chip must move off the 1.0.1 `default.csv` app slot (`0x140000`) onto `min_spiffs.csv` (`0x1E0000`). Do that before `SERIAL` / `SECRET`. Do not send an image larger than `0x140000` to a unit still on `default.csv`. NVS (`0x9000` / `0x5000`) and otadata offsets match in both tables; erase and merged flash are still forbidden once identity exists.

No secrets are compiled in. `include/secrets.example.h` is the template. `include/secrets.h`
is gitignored; the 1.0.1 copy that held a real API key is not on this branch. Provision with
the USB console or `tools/provision.py`. Rotate that old API key if a 1.0.1 unit used it.

There is no zip packager in this repo. A source zip must omit `include/secrets.h`, `.pio/`,
`*.env`, `*provision*.env`, and `.powercell-dev-*` (all gitignored). Do not add binaries
that were built with a compiled-in key.

`lib/PowercellOTA` and `include/websocket_ca.h` stay so `smart-socket-firmware/` still builds.
The collector env sets `lib_ignore = PowercellOTA` (OTA for 2.5.0 is `src/ota.cpp`).

What the Powercore app needs from this firmware:

- SoftAP `Powercell-NNNNNN` from the factory serial `ACE-NNNNNN` (MAC-based `Powercell-XXXXXX` only before the station writes a serial), per-unit hotspot password.
- Local HTTP `/v2/*` on port 80, including `POST /v2/login`, `POST /v2/setup/passcode`, `GET /v2/status`, and the network routes (`/v2/wifi/scan`, `/v2/wifi/test`, `/v2/wifi/status`, `/v2/wifi/save`, `POST /v2/network_mode`).
- Cloud and OTA HMAC: `X-Device-ID`, `X-Timestamp`, `X-Nonce`, `X-Signature` = HMAC-SHA256 of the factory `device_secret` over `METHOD\nPATH?query\nts\nnonce\nhex(sha256(body))`. While `POWERCELL_LEGACY_SECRET_HEADER` stays 1, the upgrade also sends `x-device-secret`.
- `payment_lock` survives an as-bought factory reset. User on/off is refused with `payment_locked` until a signed unlock.
- As-bought factory reset (`POST /v2/factory_reset` or console `FACTORY_RESET` twice) wipes owner, passcode, phones, Wi-Fi, and sockets, and keeps serial, device secret, label hotspot password, `api_key`, and `ota_url`.
- `hotspot_only` (`POST /v2/network_mode` `{"mode":"hotspot_only"}`) leaves the SoftAP up, does not join a router, and does not open the cloud socket.

The AcePlug socket in `smart-socket-firmware/` uses the same 433 MHz profile as this collector. See [Smart socket](#smart-socket). KyleCI PR #1 is the review branch. Do not merge it into QGatePass/Acecore until both the collector `esp32dev` build and the socket `esp32dev` build are green.

The notes below are the v17 firmware write-up. The host table in [Cloud hosts](#cloud-hosts) is the current contract: Upload is api-dev.

---

# Acecore Powercell collector firmware 2.0 (ESP32)

A clean rewrite of the ESP32 "collector" firmware for the Powercell (an inverter and battery product). It replaces:
- **Aug–Sep 2026 (Drive) firmware:** STM32 UART text link at 9600 baud and a JK BMS.
- **Oct 2026 firmware, 1.0.1 to 1.0.4:** a PZEM-004T meter, a battery voltage divider, ESP-NOW smart sockets and HTTPS OTA. The newest source is `acecore-collector-now 2.zip` (1.0.4, 14 Oct).

The collector speaks the backend WebSocket and OTA contract documented below, and the same LoRa profile as `smart-socket-firmware/` (see [Smart socket](#smart-socket)).

## Build, test, flash
```
pio run                      # env esp32dev (live api-dev.acecore.tech; OTA model esp32dev; version 2.5.0)
pio run -e esp32dev_prod     # production api.acecore.tech — not the Upload default
pio run -e esp32dev_framed   # STM32 binary protocol at 115200, still api-dev
pio run -e esp32dev_nolora   # no LoRa radio (frees IO26), still api-dev
pio run -e esp32dev_dev      # api-dev, bare board, battery ADC off
make -C test/host            # host unit tests
pio run -t upload && pio device monitor
```
Toolchain: espressif32@7.0.1, Arduino-ESP32 2.0.17 (IDF 4.4), the same as 1.0.x.
`pio run -t upload` is app-only. After identity exists, do not erase and do not flash a merged bin (see Flash rules at the top).

## Cloud hosts

`platformio.ini` is the host contract. Upload with no env selected is `esp32dev`.

| Env | Selected by Upload? | WebSocket | OTA base | Notes |
|---|---|---|---|---|
| `esp32dev` | yes (`default_envs`) | `wss://api-dev.acecore.tech/ws/device` port 443 | `https://api-dev.acecore.tech` | Live cloud. Model `esp32dev`. Version 2.5.0. |
| `esp32dev_framed` | no | same as `esp32dev` | same as `esp32dev` | `STM32_LINK_PROTOCOL=1` |
| `esp32dev_nolora` | no | same as `esp32dev` | same as `esp32dev` | `FEATURE_LORA=0` |
| `esp32dev_dev` | no | same as `esp32dev` | same as `esp32dev` | Bare board. `POWERCELL_DEV_BUILD=1`, `FEATURE_BAT_SENSE_ADC=0`. Still TLS. |
| `esp32dev_prod` | no | `wss://api.acecore.tech/ws/device` port 443 | `https://api.acecore.tech` | Production. Pick it only on purpose. Same model string `esp32dev`. |

- An NVS `ota_url` overrides the compile-time OTA base and must be the host the unit is enrolled on (`SET ota_url` / `tools/provision.py`).
- No checked-in env is cleartext. `POWERCELL_DEV_INSECURE` is the only way to get `ws://` or `http://` OTA, and `config.h` fails the build if that flag is set without `POWERCELL_DEV_BUILD`. This tree does not point any env at a raw IP.
- IO34 battery sense is off only on `esp32dev_dev` (bare board). The backend's `set_local_token` is refused on cleartext, so a local token is provisioned over USB.
- No secrets are compiled in. `tools/provision.py --port COMx --file <KEY=VALUE file>` sends `SET api_key`, `wifi_ssid`, `wifi_pass`, `gw_name`, `device_secret`, `ota_url`, `local_tok` over the USB console and prints only OK/ERR and the token fingerprint. Provisioning files are gitignored. (v17: one factory identity secret; `DEV_SECRET` / `SET dev_secret` removed.)
- WebSocket keepalive: client ping every 15 s, pong timeout 10 s, disconnect after 2 misses; server pings are answered in `ws.loop()`. The periodic OTA check runs alongside the socket; only an actual install pauses it (`lib/pc_core/src/ota_gate.h`). A main-loop pass over 500 ms logs `LOOP SLOW`, and `LOOP STATS` reports the worst pass each minute.
- Bare-board behaviour: missing ADS1115/LoRa/SD/STM32/BMS are logged once; battery `source` is `none`; an ON command without a live STM32 is rejected at once with `stm32_timeout` and IO15 stays LOW (`lib/pc_core/src/power_policy.h`, host-tested).

## Architecture
```
lib/pc_core/         hardware-independent, host-tested
  crc               CRC-16/CCITT-FALSE, CRC-16/MODBUS, CRC-32/IEEE
  jk_bms            JK-PB 300-byte frame assembler + DYNAMIC parser (checksum, plausibility)
  stm32_frame       0xAA|LEN|VER|TYPE|SEQ|PAYLOAD|CRC16 codec + status v1 payload
  stm32_legacy_parser  bounded line parser for today's STM32 printf telemetry
  ring_queue        fixed-capacity FIFO
  identity          MAC formats, gateway name, AP SSID/password, URL encoding
src/
  main.cpp          setup order + cooperative loop
  settings          identity + NVS-only secrets, migration from 1.0.x NVS
  stm32_link        Serial1 link, pluggable parser (legacy text | framed), commands
  stm32_updater.h   STM32 OTA interface stub (bootloader_protocol.md), paused
  bms               JK-PB BMS listener on Serial2 (IO16/IO17, RS485 module)
  battery           merges BMS (owner) / STM32 PA3 (cross-check, fallback) / IO34 BAT_SENSE
  solar             ADS1115 PV voltage/current (ported from Dubem mppt.cpp)
  periph            shared SPI bus, ADS1115 + LoRa bring-up, optional SD card probe
  lora_link         SX1278 433 MHz bring-up (socket protocol not ported yet)
  gateway           inverter on/off request to the STM32: set_mode (framed) or PWR_KEY IO15 (legacy)
  sockets/          ESP-NOW smart sockets (vendored from 1.0.4, keys from NVS)
  telemetry         JSON builders (WS telemetry/snapshot, local /metrics)
  backend_ws        WSS client, command handling, idempotency cache
  offline_queue     bounded RAM ring, NVS checkpoints, batched flush + backoff
  ota               HTTPS self-update with rollback (ported PowercellOTA)
  net               non-blocking Wi-Fi STA + per-unit setup AP
  web_local         authenticated local HTTP API (POST for changes)
  console           USB factory/bench console
  leds              status LEDs
```

From 2.2 onward the active smart-socket transport is LoRa (`src/sockets/sockets_lora.cpp`, `FEATURE_LORA_SOCKETS`). ESP-NOW stays in the tree and is compiled out (`FEATURE_ESPNOW_SOCKETS=0`). The diagram above is the 2.0 layout.

Tasks:
- `stm32_rx` (prio 3)
- `stm32_poll` (framed mode only)
- `bms`, `batsense`, `solar` (all on core 1)
- `socket_link` (ESP-NOW)
- `ota` worker (on demand)
- Async web/TCP

Everything else runs in `loop()`, which never blocks for long. The exception is the console `STM32 MODE` command, which can block for up to 0.8 s on the bench.

## Pin map (ESP32-WROOM-32, C:U10)
Source of truth: the hardware VP's `schematic_review.md` (sections 1 and 5), `Collector.kicad_sch` and the Dubem reference firmware (`include/pinDefines.h`).

| GPIO | Function | Notes |
|---|---|---|
| 0 / 1 / 3 | Auto-program (Q9) / UART0 to PROG_HEADER | USB console 115200 |
| 33 | Serial1 RX ← STM32 PB6 (USART1 TX, remapped) | |
| 2 | Serial1 TX → STM32 PB7 (USART1 RX) | Strapping pin; becomes TX only after boot. Never used for LoRa DIO0 |
| 16 / 17 | Serial2 RX / TX ↔ TTL-RS485 module ↔ JK-PB BMS | Header labelled "PZEM-004T" on the schematic. Auto-direction module, no DE/RE |
| 15 | PWR_KEY → R57 1k → Q7 → STM32 PB4 (CONT) | HIGH turns Q7 on, PB4 LOW = run the inverter. Strapping pin: driven LOW first thing in `setup()` |
| 34 | BAT_SENSE (LM324 buffer, same node as STM32 PA3) | Input only; last-resort pack voltage |
| 21 / 22 | I2C SDA / SCL (C:J21, 1k pull-ups) → ADS1115 | Fitted; PV voltage/current |
| 18 / 19 / 23 | SPI SCK / MISO / MOSI shared by SD, LoRa, MPPT SPI | |
| 4 / 26 | LoRa CS3 (C:J16 pin 7) / LoRa RESET (C:J16 pin 3) | SX1278 433 MHz, DIO0 not used (polling) |
| 14 | SD card CS2 | Optional, detected at boot |
| 5 | MPPT SPI CS | Held high (deselected); not used |
| 13 / 12 | LED red / yellow (BC337) | Old-code mapping (`LED_MAP_OLD_CODE=1`). The schematic labels IO12 "R" and IO13 "Y"; set 0 if Caleb confirms the schematic. IO12 is the flash-voltage strap |
| 27 / 32 / 25 | LED green / amber / PWR_LIGHT (BC337) | Amber: BMS alarm (steady) or BMS silent (blink). PWR_LIGHT follows the inverter request |
| 35 / 36 / 39 | Unused, input only | |

There is no RTS/CTS on the STM32 link. The ESP drives **no relay**: the STM32 owns the relay and the inverter. `pins.h` fails the build if LoRa and the IO26 NRST bodge are both enabled.

## Data ownership
| Data | Owner | Where it appears |
|---|---|---|
| SoC, cell voltages, pack current/power, cell and MOS temperatures, cycles, SOH, capacity, BMS alarms, MOS states | JK-PB BMS | `battery.*` |
| Pack voltage shown | BMS (`battery.voltage`); STM32 PA3 kept as `battery.stm32_voltage`, with `battery.voltage_mismatch` when they differ by > 1.5 V | `battery.*` |
| Inverter output V, AC load current (CT), grid V, mode, relay/transfer, NTC temperature, charger state, faults | STM32 | `inverter.*` |
| PV voltage / current / power | ADS1115 | `solar.*` |

When the BMS is silent for 10 s, every BMS field is `null` (never 0) and voltage/SoC fall back to the STM32 (then IO34). Low/critical battery alerts follow the shown SoC, so they come from the BMS, or from the STM32 when the BMS is offline.

## Decisions
1. **Base:** 1.0.4 (`acecore-collector-now 2.zip`) for the backend, OTA and sockets; the Dubem collector firmware is the hardware reference for the BMS, ADS1115 and LoRa.
2. **UART allocation:** UART0 USB console; UART1 STM32; UART2 JK-PB BMS through the RS485 module. **There is no PZEM-004T**: the driver, its library and the ESP-side energy accounting are removed.
3. **JK-BMS:** JK-PB series only. The BMS streams fixed 300-byte frames `55 AA EB 90 | type | ...`; type `0x02` DYNAMIC is parsed, `0x01` SETUP is skipped. The ESP only listens (115200 8N1, `JK_BMS_BAUD`). Each frame's checksum (byte 299 = sum of bytes 0..298) is checked; bad or implausible frames are counted and dropped, never published.
4. **Smart sockets:** ESP-NOW (1.0.4 wire format) stays the active transport. The LoRa radio (433 MHz, sync 0xF1, SF9/125 kHz/4:5/preamble 12, as the Dubem profile) is brought up in the main build, but the Dubem LoRa socket protocol is not ported yet.
5. **STM32 protocol:**
   - The default is `legacy_text` at 9600 (what the STM32 sends today), using a bounded 96-byte line buffer.
   - `framed` at 115200 is fully implemented: parser, encoder, CRC16, sequence numbers, retries, get_status / set_mode (ack/nack) / fault_reset / get_version / enter_bootloader.
   - To switch modes, build `-D STM32_LINK_PROTOCOL=1`, or send `SET stm32_proto 1` over USB and reboot. **Switch only together with the STM32 release.**
   - `enter_bootloader` is refused locally unless the last status reported SYS_IDLE. The STM32 checks again.
6. **Status payload v1 (30 bytes, LE) is agreed with the STM32 side** (`stm32_frame.h`): sys, switch, charge, restart_count, u32 fault_bits, bat_mV, bat_pct, transfer_flags (bit0 transfer active, bit1 grid-phase synced, bit2 VOUT_SCALE_CALIBRATED), grid_dV, temp_dC, vout_dV, iout_cA, chg_cA, mod_x10000, uptime_s, valid_flags (bat/grid/temp/output). `set_mode`: the STM32 ACKs only once the mode is applied (ESP32 waits up to `STM32_SET_MODE_TIMEOUT_MS`, 3 s) and NACKs with `ST_BAD_STATE` while a fault is latched. Switch-over to framed waits for STM32 v2 bench sign-off.
7. **STM32 OTA is paused.** Only `stm32_updater.h` exists; every call returns NotImplemented.
8. **Power on/off ("gateway") goes through the STM32.** Framed builds send `set_mode INVERTER` / `OFF` and only confirm the backend command after the STM32 ACKs (rejections: `stm32_rejected`, `stm32_timeout`, `stm32_busy`, `refused`). Legacy builds drive PWR_KEY (IO15 HIGH = request inverter). The local `POST /state` queues the request and it is applied from `loop()`. IO15 boots LOW, so after a reset the inverter is not requested until the backend or app asks again.
9. **No energy counter on the ESP.** Without a PZEM there is no real AC power meter; `V x I` from the STM32 is reported live as `inverter.output_power` (apparent power) but not integrated.

## What changed and why (fix list)
| Problem in 1.0.x | 2.0 |
|---|---|
| A real API key in `include/secrets.h` (in every zip, including 1.0.4); ESP-NOW keys in `espnow_pairing.h` | No secrets in source. Secrets are provisioned into NVS (`pc_secure`). `secrets.h` is gitignored and `secrets.example.h` is a template |
| Shared setup AP password (`12345678` / `powercell123`) | Per-unit SSID `Powercell-XXXXXX` and a random per-unit password, or a provisioned `ap_pass`, printed on the label |
| Unauthenticated GET `/ON`, `/OFF`, `/provision`, `/relay`, `/register` | HTTP Basic `powercell:<local_access_token>` on everything except `/` and `/api/health`; state changes are POST only (405 otherwise), see Local API |
| OTA disabled in 1.0.1 (`main.cpp:94`) | OTA enabled. The image is marked valid after 30 s of continuous backend connection, and rolls back if it isn't healthy within 5 min or reboots first (bootloader rollback is enabled in this core) |
| Offline queue: JSON re-parse and NVS rewrite on every event, resend every 2 s | RAM ring of 32. NVS checkpoint at most every 30 s (lifecycle events sooner). Batches of 8. Backoff from 2 s to 60 s |
| WS silently waited for NTP | Connects without NTP: this mbedTLS build doesn't check certificate dates. Logs `NTP_WAIT` every 10 s until the clock is set; only command expiry waits for it |
| `/metrics` `device_state` was always true; unmeasured values were 0 | Real inverter request state; `null` when not measured; adds `stm32_online`, `battery_source`, `bms_online` |
| LoRa DIO0=IO2 collides with the STM32 TX | DIO0 unused (polling), compile-time check that it is never IO2 |
| Relay driven on IO15 | IO15 is PWR_KEY to the STM32 PB4; the ESP drives no relay |
| PZEM-004T code although no PZEM is fitted | Removed; UART2 now reads the JK-PB BMS |
| Hard-coded `ACE4447` / `ACE9678` gateway name | `ACE-XXXXXX` from the MAC, overridable with `SET gw_name` |
| One pending socket command | Up to 4 concurrent commands (one per socket), matched by command sequence. Retried command_ids return `duplicate` |
| `Serial1.begin` called twice; unbounded `String` line buffer | Single begin; fixed 96-byte buffer with overflow counter |

## Backend contract (official-powercell-app PR #3)
- **WebSocket `/ws/device`:**
  - URL: `wss://<host>/ws/device?device_id=AA%3ABB%3A...` (MAC with colons)
  - Headers: `API-KEY: <API_KEY>`, `X-Device-ID: AABBCCDDEEFF`, and `X-Device-Secret` when the factory identity secret is set (console `SECRET` / `SET device_secret`; optional until the backend sets `DEVICE_SECRET_REQUIRED=true`). Backend field name remains `device_secret`.
  - Every message carries `gateway_id` (colon MAC) and `device_id` (12 hex). The backend treats colon, hyphen and bare MACs as one device; a legacy name such as `ACE4447` must have the MAC as an alias (provision with `{"device_id":"ACE4447","mac":"..."}`).
- **Commands in:** `command` (PR #3 wire name) and `set_state` (legacy) are handled the same way: `{"type":"command","command_id","target_type":"gateway"|"socket","target_id":null|"<socket>","desired_state":"on"|"off"}`. A gateway command with `target_id` null targets this unit.
- **Command ack out:** `{"type":"command_result","command_id","status":"confirmed"|"rejected","command_status":same,"error_message"?, ...}`. `status`/`command_status` is what the backend reads; `result`, `reason` stay for older servers. A repeated command id answers with the earlier outcome.
- **Also in:** `ping`, `socket_lifecycle`, `offline_sync_ack`, `set_local_token` (below).
- **OTA:** `POST {ota_url}/api/ota/check` with `Authorization: Bearer <API_KEY>`, `X-Device-ID`, optional `X-Device-Secret`, body `{"device_id":"AABBCCDDEEFF","firmware_version":"2.5.0",...}`. The response must have `update_available`; when true, `version`, `url`, `sha256` (top level or in `firmware`; they must agree). `sha256` is required (64 hex). The `url` is followed only if it is on the configured `ota_url` host and is one of `/api/ota/download`, `/api/firmware/download`, `/api/firmware.bin`; otherwise `{ota_url}/api/ota/download?device_id=<12 hex>` is used. The download must send `Content-Length`. The image is checked for SHA-256, the ESP32 image magic and the embedded identity `POWERCELL_FW|collector|esp32dev|<version>|END`, then boots in rollback-protected PENDING_VERIFY.

## Local API (port 80, PR #3 "Local device web server")
Credential: HTTP Basic `powercell:<local_access_token>`. The token comes from the backend (see "Local token handoff"). The label `admin_tok` is a **setup credential only**: Basic `powercell:<label token>` works only from a client on the setup AP, and only for `GET /`, `POST /login`, `POST /provision`.

| Endpoint | Auth | Behaviour |
|---|---|---|
| `POST /login` | header and/or body `{"username":"powercell","token":"..."}` | 200 `{ok,username,device_id,scope:"device"\|"setup"}`, else 401 |
| `GET /` | token | Status page. Without a credential: setup page on the AP, or when no token exists yet; otherwise 401 with a Basic challenge |
| `GET /data` | token | JSON: `device_id`, `mac`, `device_state` (`On`/`Off`), `state`, `battery{valid,status,percentage,voltage,age_ms,source}`, `metrics{...}` (see below), `notifications[]` |
| `GET /events` | token | SSE: `ready` on connect, `state` (on change and every 10 s), `heartbeat` (every 10 s), `notification` (battery_low/critical/shutdown_imminent, battery_reading_unavailable, with `id`, `sequence`, `active`) |
| `POST /state` | token | `{"state":"on"\|"off"}` → `{ok,state,applied_via:"stm32_set_mode"\|"pwr_key"}`; applied from `loop()` |
| `POST /relay` | token | `{"socket_id":"...","state":"on"\|"off"}` → 202 |
| `POST /register` / `/unregister` | token | `{"socket_id":"..."}` (also `id`) |
| `POST /provision` | token, or setup on AP | `{"ssid","pass"\|"password","local_token"?}`; Wi-Fi change reboots |
| `GET /api/health` | none | version, device_id, `local_token_set` |
| `GET /metrics`, `GET /api/state`, `POST /api/inverter/mode`, `POST /api/inverter/fault_reset` | token | unchanged |

POST-only routes answer **405** (with `Allow: POST`) to any other method before checking auth; a missing or wrong credential is **401**. JSON bodies up to 1 KB; form-encoded bodies are also accepted. The old `GET /ON`, `/OFF` and `/api/gateway` etc. routes are gone.

**Before a token exists:** only `GET /`, `GET /api/health`, and (from the setup AP with the label token) `POST /login` and `POST /provision` work. `/data`, `/events`, `/state`, `/relay`, `/register`, `/unregister` return 401 for everyone.

## Local token handoff
1. **Preferred, WebSocket** (backend must add it; PR #3 has no such message yet). After the device authenticates on `/ws/device`, the backend sends, whenever its sealed token is not yet confirmed for this device (first connect after provisioning, and after `POST /api/devices/local-access/rotate`):
   ```json
   {"type":"set_local_token","request_id":"<uuid, max 47 chars>","username":"powercell","token":"<local_access_token>"}
   ```
   The device validates (`username` must be `powercell`; token 32 to 128 chars of `[0-9A-Za-z_-]`; TLS builds only), stores it in NVS `pc_secure/local_tok` (same token again = no write), uses it at once, and replies:
   ```json
   {"type":"local_token_ack","request_id":"<same>","status":"confirmed"|"rejected","error":"invalid_token|invalid_username|invalid_request_id|insecure_transport|storage_failed","token_fingerprint":"<first 16 hex of sha256(token)>","device_id":"AABBCCDDEEFF","gateway_id":"AA:BB:...","firmware_version":"..."}
   ```
   The backend marks the token delivered when `status` is `confirmed` and `token_fingerprint` equals `substr(hash('sha256',$token),0,16)`; otherwise it resends on the next connect. The ack has no `command_id`, so it does not touch the command table.
2. **USB fallback:** `SET local_tok <token>` (works after `LOCK`; prints the fingerprint). `OTA_CONFIG {"local_token":"..."}` also works before `LOCK`.
3. **Setup AP fallback:** `POST /provision` with Basic `powercell:<label token>` from the AP and `{"local_token":"..."}` (with or without `ssid`).

## Provisioning (factory, USB console 115200)
```
SET api_key <shared hardware API_KEY>   # also the OTA Bearer token
SET device_secret <64 hex>              # optional rework; new units use console SECRET (factory identity)
SET local_tok <local_access_token>      # optional; normally delivered over the WebSocket
SET ota_url https://api-dev.acecore.tech   # must match the server's APP_URL host
SET espnow_keys <PMK32hex><LMK32hex><AUTH32hex>   # same values as the socket fleet
SET wifi_ssid <ssid>      (optional; or use the setup AP)
SET wifi_pass <password>
INFO                      # prints LABEL ap_pass=... admin_token=... for the label printer
LOCK                      # secrets can no longer be read or changed over USB (local_tok excepted)
REBOOT
```
`OTA_CONFIG {json}` from 1.0.x is still accepted (`token` is ignored). `FACTORY_RESET` (typed twice) on 2.5.0 is the as-bought reset: it keeps factory identity. It does not wipe the serial or device secret.

## Migration notes (1.0.x → 2.0 by OTA)
- **Secrets:** on first boot, 2.0 copies the 1.0.x NVS values into `pc_secure`:
  - `powercell_ota/api_key` and `url` (the 1.0.x 64-hex OTA `token` is no longer used: PR #3 OTA takes `API_KEY` as Bearer)
  - `wifi/ssid` and `pass`

  So units keep their backend identity with no USB visit.
- **ESP-NOW keys are NOT migrated.** They were compiled into 1.0.x. Before shipping 2.0 by OTA, either provision `espnow_keys` over USB, or add a one-off migration build that writes them to NVS (see the open questions below). Without them, sockets show as offline.
- **Gateway name:** it changes from `ACE4447` / `ACE9678` to `ACE-XXXXXX` unless you `SET gw_name ACE9678` on that unit. The backend should accept MAC-based gateway targets, which 2.0 accepts.
- **Network:** the static IP from 1.0.x is replaced by DHCP. The setup AP SSID changes to `Powercell-XXXXXX`.
- **Not migrated:** the old `energy_wh`, and the 2.0.0 ESP energy counter (`pc_energy` NVS) is no longer used; the old offline queue (JSON in `offline_sync/events`).
- **Version:** 2.5.0 is reported as `firmware_version`. Publish it under kind `collector`, model `esp32dev` (that model string is unchanged on `esp32dev_prod`).
- **STM32 link:** the STM32 still sends 9600 text, so the default `legacy_text` works with today's STM32 firmware. Switch both sides to framed together.

## Telemetry fields (WS `telemetry`, additive to 1.0.x)
- `battery`: `voltage` (BMS, else STM32, else IO34), `soc`, `source` (`bms`\|`stm32`\|`adc`\|`none`), `stm32_voltage`, `voltage_mismatch`, `bms_online`, `bms_age_ms`, `current` (A, + charging), `power` (W), `temperature`, `temperatures[2]`, `mos_temperature`, `cycles`, `soh`, `remaining_ah`, `full_ah`, `status` (`standby`\|`charging`\|`discharging`), `charge_mos`, `discharge_mos`, `balancing`, `cells[]` (V), `cell_min_v`, `cell_max_v`, `cell_diff_mv`, `alarm_bits`, `alarms[]` (names). BMS fields are `null` (arrays empty) when the BMS is offline.
- `inverter` (STM32 only): `stm32_online`, `requested_on`, `output_voltage`, `output_current`, `output_power` (V x I), `grid_voltage`, `grid_available`, `temperature`, `battery_voltage` (PA3), `system`, `switch`, `charge_state`, `fault_code`, `faults[]`, plus the framed-only extras.
- `solar`: `online`, `voltage`, `current`, `power` (null when the ADS1115 is silent).
- `system.peripherals`: `ads1115`, `lora`, `sd_card`, `sd_size_mb`.
- **Removed:** `inverter.pzem_ok`, `inverter.energy_kwh`, `inverter.frequency`, `inverter.power_factor`, `inverter.output_energy_wh`, `inverter.energy_source`; `/metrics` `pzem_ok`, `inverterFrequency`, `inverterPf`, `inverter_energy`; `/api/state` `energy_output_wh`.
- `/metrics` adds `battery_voltage_stm32`, `bms_online`, `battery_current`, `battery_power`, `battery_temperature` (now real), `bms_mos_temperature`, `battery_cycles`, `bms_alarm_bits`, `sd_card`; `lora_ready` now means the LoRa radio answered.

## Status
| Area | Status |
|---|---|
| Builds (esp32dev live api-dev, esp32dev_prod, esp32dev_framed, esp32dev_nolora, esp32dev_dev), firmware 2.5.0 | App slot is 0x1E0000 (`min_spiffs.csv`), not the old 1.25 MB slot |
| Host tests | `make -C test/host` |
| STM32 legacy parser, framed protocol and commands | Done. Bench test against real STM32 output is pending |
| Backend WS, OTA with rollback, offline queue, local API, provisioning | Done. On-device testing is pending |
| JK-PB BMS (RS485) | Done. Tested on synthetic frames; needs a bench capture from the fitted BMS |
| ADS1115 solar | Ported from Dubem. Calibration (`VCAL`) on the bench |
| ESP-NOW sockets | Ported 1.0.4 logic. Needs a regression test with socket 1.0.1 |
| LoRa | Radio bring-up only. The Dubem LoRa socket protocol is not ported |
| SD card | Detected only; nothing is logged to it yet |
| STM32 OTA (bootloader client) | Stub only, paused |
| SPIFFS dashboard | Not ported. It needs updating to the authenticated POST API |

## Firmware 2.1: offline-first onboarding

Spec: `/workspace/esp32_review/onboarding_spec.md` (section 13 WS messages frozen).
Decisions: 6-digit numeric passcode (not all one digit, not the label value); factory reset keeps the owner;
`unclaim` with `transfer`/`support` clears paired phones, `owner_request` keeps them; hotspot fallback after
`AP_FALLBACK_MIN` = 5 min plus console `AP ON`.

- Pure logic (host-tested): `lib/pc_core/src/sha256.*` (SHA-256, HMAC, PBKDF2), `onb_core.*` (passcode verifier,
  fingerprint, claim hash, limiter, paired phones, sessions, hotspot policy, WS rules).
- Glue: `src/onboarding.*` (NVS `pc_onb`, `pc_factory`), `src/web_v2.cpp` (`/v2/*`), `src/net.cpp` (live Wi-Fi test,
  scan, hotspot policy), `src/backend_ws.cpp` (`device_hello`, `passcode_changed`, `set_passcode(_ack)`,
  `claim_result`, `unclaim`, 4401 back-off).
- Verifier: PBKDF2-HMAC-SHA256(passcode, 16-byte salt, 10000, 32). Fingerprint: first 32 hex of
  HMAC-SHA256(factory device_secret, the 32-byte PBKDF2 output). Claim hash: SHA-256 hex of `<12-hex device_id>|<code>`.
- `/v2/wifi/scan` returns cached results with `"scanning":true` when it started a fresh scan (poll again in ~3 s).
  `/v2/wifi/test` accepts an optional `"phone_sees_5ghz":true` hint for the `5ghz_only` reason.
- Console: `SET passcode <6 digits>` (factory, before LOCK), `INFO` prints `LABEL PWC1;S=..;P=..;D=..;C=..` before
  LOCK, `AP ON`, `FACTORY_RESET` (spec section 8), `FACTORY_WIPE` (erase everything; replaces the old FACTORY_RESET).

The onboarding spec path above is not in this repo. v15 changed factory reset: an as-bought reset wipes the owner (see the top of this file). `labelLine()` now prints `PWC2`.

## Firmware 2.5.0 (v15–v17)

- **v15 factory identity.** NVS namespace `pc_ident`: serial `ACE-NNNNNN`, 32-byte `device_secret`, label hotspot password. The SoftAP SSID is `Powercell-` plus the six serial digits. HMAC for the WebSocket upgrade, OTA, and the passcode fingerprint uses that secret (`src/devauth.cpp`, `src/factory_id.cpp`).
- **v16 payment_lock.** Signed HQ command. The latch survives factory reset and `FACTORY_WIPE`. Console `PAYMENT_UNLOCK` is the bench clear. Local and cloud on/off return `payment_locked` while the latch is set.
- **v17 secret merge.** First boot copies a 64-hex NVS `device_secret`, or else a 64-hex `dev_secret`, into `pc_ident` and then deletes the old keys. `SET dev_secret` returns an error and tells the operator to use `SECRET` or `SET device_secret`.

## Smart socket

Caleb 2026-10-26: collector and AcePlug sockets ship together on this KyleCI branch. `smart-socket-firmware/` is the v0.1 image (433 MHz, not the old 915 MHz SF7 / 50 A firmware). QGatePass/Acecore `main` stays untouched until this PR shows both the collector `esp32dev` build and the socket `esp32dev` build green. Do not merge it there from this branch.

Both sides use `include/powercell_lora_profile.h`:

| Setting | Value |
|---|---|
| Frequency | 433 MHz |
| Spreading factor | 9 |
| Bandwidth | 125 kHz |
| Coding rate | 4/5 |
| Preamble | 12 |
| Sync word | `0xF1` |
| CRC | on |
| TX power | 20 dBm |
| SPI clock | 2 MHz |

Commands: `<id>,RELAY_ON,<cmdId>`, `<id>,RELAY_OFF,<cmdId>`, `<id>,RESET_LOCKOUT,<cmdId>`, `<id>,SET_LIMIT,<deciamps>,<cmdId>`. The fitted contact is **10 A**. Software protection is warn at 8 A (clear below 7 A) and trip at 10 A. User limits are 1.0–10.0 A (10–100 deciamps). This is not a 20 A SKU. Lockout is 3 trips in 5 minutes. Telemetry adds `ACK`, `LIM`, `WARN:CURRENT_HIGH`, and `ALERT:LOCKOUT`.

**AcePlug v0.1 pins** are CAD-verified from Smart Socket.kicad, U3 ESP32-WROOM-32. They are locked in `smart-socket-firmware/src/main.cpp`:

| Signal | GPIO | Note |
|---|---|---|
| `RELAY_PIN` / SOCKET_RELAY | **15** | CAD-verified. Not v0.2 IO27. |
| `LORA_CS_PIN` (NSS) | 5 | CAD-verified |
| `LORA_RESET_PIN` | 25 | CAD-verified |
| `LORA_DIO0_PIN` | 26 | CAD-verified |
| `LORA_SCK_PIN` / `LORA_MISO_PIN` / `LORA_MOSI_PIN` | 18 / 19 / 23 | CAD-verified |
| `LED_PIN` | 4 | CAD-verified |
| `CURRENT_PIN` | 35 | ZMCT103 CT through EmonLib. Not BL0942. |
| `RESET_BUTTON_PIN` | 0 | Module BOOT button. Not a U3 net in this list. |

The collector header is different (CS IO4, RESET IO26, DIO0 not wired). The socket does not include `include/pins.h`.

The socket schematic symbol is **RFM95W-915S2**. This firmware is 433 MHz, sync `0xF1`, SF9. The module fitted on the board has to be a 433 MHz part. A 915 MHz module will not hear the collector.

Do not apply the GPIO14 / 4 / 2 / 33 / 22 / 21 list in `LORA_SOCKET_PROTOCOL.md`. That note is not the collector and not AcePlug.

**v0.2 is deferred** and is not compiled: RELAY 27, LoRa CS 32, RST 26, DIO0 34, BL0942 UART on IO16/IO17. Do not flash a v0.2 board with this image.

IO15 is an ESP32 strapping pin. `setup()` drives it low before other subsystems so the coil stays off through boot.

Build the socket from its own directory (`pio run` in `smart-socket-firmware/`). It uses `../lib/PowercellOTA` and `../include/websocket_ca.h`. Version string is 2.5.0, kind `socket`. There is no machine-specific `upload_port`.

## Removed from this branch

The 1.0.1 SPIFFS UI (`data/`), the 1.0.x binaries under `dist/`, and the PHP stubs
(`turn-on-device.php`, `websocket-server.php`) are deleted here. They are not in `legacy/`.
Firmware 2.5.0 does not serve that UI. Those files remain on `main` history.

## Kept from the 1.0.1 tree

These notes describe 1.0.1, not the 2.5.0 collector contract:

- `ESP32_PRODUCTION_INTEGRATION.md`, `WEBSOCKET_SERVER_INTEGRATION.md` — 1.0.1 WebSocket notes. The live collector host is api-dev, not the host written in those files.
- `OTA_SETUP_GUIDE.md`, `OTA_UPDATES.md` — 1.0.1 USB/OTA steps. 2.5.0 OTA uses the API key as Bearer, checks the embedded `POWERCELL_FW|collector|esp32dev|2.5.0|END` identity, and needs the `min_spiffs.csv` layout. After identity exists, do not erase and do not flash a merged bin.
- `LORA_SOCKET_PROTOCOL.md` — an old binary RadioLib packet. The socket that ships here is the ASCII 433 MHz profile in [Smart socket](#smart-socket).
- `OFFLINE_BATTERY_ALERTS.md`, `FLUTTER_WEBVIEW_OFFLINE_NOTIFICATIONS.md`, `ANALYSIS.md` — 1.0.1 product notes.

`include/pinDefines.h` from 1.0.1 was removed. The collector pin map is `include/pins.h`.
