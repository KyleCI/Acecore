# Acecore Powercell collector firmware 2.0 (ESP32)

A clean rewrite of the ESP32 "collector" firmware for the Powercell (an inverter and battery product). It replaces:
- **Aug–Sep 2026 (Drive) firmware:** STM32 UART text link at 9600 baud and a JK BMS.
- **Oct 2026 firmware, 1.0.1 to 1.0.4:** a PZEM-004T meter, a battery voltage divider, ESP-NOW smart sockets and HTTPS OTA. The newest source is `acecore-collector-now 2.zip` (1.0.4, 14 Oct).

The new firmware still works with the existing backend, the socket firmware 1.0.1 and the OTA pipeline.

## Build, test, flash
```
pio run -e esp32dev_stm01_5kw      # STM32, model pc-stm01-5kw
pio run -e esp32dev_stm01_35kw     # STM32, model pc-stm01-35kw
pio run -e esp32dev_dsp011_5kw     # DSP, model pc-dsp011-5kw
pio run -e esp32dev_dsp011_35kw    # DSP, model pc-dsp011-35kw
make -C test/host                  # host unit tests
pio run -t upload && pio device monitor
```
Toolchain: espressif32@7.0.1, Arduino-ESP32 2.0.17 (IDF 4.4), the same as 1.0.x.

**Cloud:** the four product images use `wss://api-dev.acecore.tech` and OTA `https://api-dev.acecore.tech`. Each image's OTA model is its own `pc-stm01-*` or `pc-dsp011-*` string. See `docs/OTA_HARDWARE_MODEL.md`.

**DSP board (Caleb, 2026-11-10):** Both DSP ratings listen for JK-PB frames at 115200 on RX GPIO17 / TX GPIO16 (`DSP_BMS_JKPB=1`), as Dubem's working DSP firmware does. PZEM is UART1 RX GPIO35 / TX GPIO32 at 9600 8N1 and is the only output V/I source (Caleb, 13:16 WAT; Dubem's working firmware uses the same pins). GPIO33 is unused on DSP. GPIO35 is input only with no internal pull-up, so the board needs a pull-up to 3.3 V on the PZEM TX line. Grid voltage is ADC1 GPIO39 only; GPIO34 is not read. GPIO15 power is HIGH = ON (`DSP_PWR_ACTIVE_LEVEL` 1), confirmed 2026-11-10 11:53 WAT. That supersedes the earlier LOW = ON note. Off is LOW, including the boot latch. Status lights (Caleb, 2026-11-12): orange and red are swapped, so red is GPIO13, orange (mid-SOC, `PIN_LED_ORANGE`, schematic Y) is GPIO12, and green stays GPIO14. GPIO12 is still driven low at boot because it is the MTDI strap. The front light stays GPIO27. Details and the ADC headroom caveat are in `docs/DSP_HW_CONFLICTS.md`.

## Provisioning
- No secrets are compiled in. `tools/provision.py --port COMx --file <KEY=VALUE file>` sends `SET api_key`, `wifi_ssid`, `wifi_pass`, `gw_name`, `device_secret`, `ota_url`, `local_tok` over the USB console and prints only OK/ERR and the token fingerprint. Provisioning files are gitignored. (v17: one factory identity secret; `DEV_SECRET` / `SET dev_secret` removed.) See `docs/API_KEY_ROTATION.md`.
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
- **OTA:** `POST {ota_url}/api/ota/check` with `Authorization: Bearer <API_KEY>`, `X-Device-ID`, optional `X-Device-Secret`, body `{"device_id":"AABBCCDDEEFF","firmware_version":"2.1.0",...}`. The response must have `update_available`; when true, `version`, `url`, `sha256` (top level or in `firmware`; they must agree). `sha256` is required (64 hex). The `url` is followed only if it is on the configured `ota_url` host and is one of `/api/ota/download`, `/api/firmware/download`, `/api/firmware.bin`; otherwise `{ota_url}/api/ota/download?device_id=<12 hex>` is used. The download must send `Content-Length`. The image is checked for SHA-256, the ESP32 image magic and the embedded identity `POWERCELL_FW|collector|<model>|<version>|END` (`docs/OTA_HARDWARE_MODEL.md`), then boots in rollback-protected PENDING_VERIFY. It is confirmed after 30 s connected to the server within 5 min, else it rolls back. In `hotspot_only` (no cloud) 3 min of healthy local run (free heap > 30 KB) confirms it instead, within the same 5 min.
- **OTA timeouts (2.5.18):** TCP connect 15 s, TLS handshake 45 s (was 10 s), socket read and body stall 30 s (was 15 s), whole body 180 s. Same values for the HTTPS check and the download.
- **OTA error codes (2.5.18).** After a failed cloud install the device reports `last_status: "failed"` and one of these in `last_error` (in `ota_check` over the WS and in the HTTPS check body). `last_error_detail` is a new optional string with the HTTP code, length or mbedTLS error (`tls=0` means it failed before TLS, i.e. DNS or TCP; `tls=-1` means the handshake timed out). Older firmware sends `download_or_verification_failed` for all of them; keep mapping that.

  | `last_error` | Meaning |
  |---|---|
  | `download_no_slot` | No inactive OTA partition |
  | `download_url_invalid` | The download URL could not be opened by the HTTP client |
  | `download_connect_failed` | The GET returned a negative HTTP code: DNS, TCP, TLS handshake or send failed. No request reached the server |
  | `download_timeout` | No response headers within the read timeout (HTTP -11), or the body took longer than 180 s |
  | `download_http_<code>` | The server answered with a status other than 200, e.g. `download_http_401`, `download_http_404` |
  | `download_size_mismatch` | No `Content-Length`, fewer than 288 bytes, or a length other than the offered `size` |
  | `download_too_large` | `Content-Length` is larger than the OTA slot (0x1E0000 on min_spiffs) |
  | `download_flash_begin_failed` | `esp_ota_begin` failed |
  | `download_stalled` | No byte for 30 s during the body |
  | `download_read_error` | A stream read failed |
  | `verify_magic` | The first byte is not the ESP32 image magic 0xE9 |
  | `flash_write_failed` | `esp_ota_write` failed |
  | `verify_size` | The body ended before `Content-Length` bytes |
  | `verify_identity` | `POWERCELL_FW\|collector\|<model>\|<offered version>\|END` not found in the image |
  | `verify_sha` | SHA-256 differs from the offer |
  | `finalize_failed` | `esp_ota_end` or setting the boot partition failed |

  Unchanged: `safe_output_storage_failed`, `interrupted_download`, `rolled_back`, `not_healthy_rolled_back`, `lan_*`.
- **Hello OTA host (2.5.18):** `device_hello` carries `ota_host` (`scheme://host[:port]` of the effective OTA base URL, no path, query or credentials; `null` if unparsable) and `ota_url_override` (true when a saved `ota_url` differs from the compiled default `https://api-dev.acecore.tech`).

## Local API (port 80, PR #3 "Local device web server")

**mDNS discovery (2.5.13, Caleb approved 2026-11-12 14:31 WAT):** once the station has an IP on the saved home network, the unit answers as `<display id>.local` (for example `ACE-000001.local`) and advertises `_http._tcp` on port 80 (`LOCAL_HTTP_PORT`) with TXT `device_id=<display id>` (same value as `GET /v2/setup/info` `device_id`), `model`, `fw` and `path=/v2/setup/info`. The hostname is the display id reduced to one DNS label (letters, digits, `-`, max 63 chars), falling back to `ACE-DDEEFF` from the MAC. Every STA got-IP re-announces. It stops while the station is down, in hotspot-only mode and during a Wi-Fi test. There is no shared `powercell.local` name, so two units on one LAN do not collide. Apps should still fall back to a /24 scan matched on `device_id`, since some routers drop multicast.
Credential: HTTP Basic `powercell:<local_access_token>`. The token comes from the backend (see "Local token handoff"). The label `admin_tok` is a **setup credential only**: Basic `powercell:<label token>` works only from a client on the setup AP, and only for `GET /`, `POST /login`, `POST /provision`.

| Endpoint | Auth | Behaviour |
|---|---|---|
| `POST /login` | header and/or body `{"username":"powercell","token":"..."}` | 200 `{ok,username,device_id,scope:"device"\|"setup"}`, else 401 |
| `GET /` | token | Status page. Without a credential: setup page on the AP, or when no token exists yet; otherwise 401 with a Basic challenge |
| `GET /data` | token | JSON: `device_id`, `mac`, `device_state` (`On`/`Off`), `state`, `battery{valid,status,percentage,voltage,age_ms,source}`, `metrics{...}` (see below), `notifications[]` |
| `GET /events` | token | SSE: `ready` on connect, `state` (on change and every 10 s), `heartbeat` (every 10 s), `notification` (battery_low at <= 25 %, battery_critical at <= 15 % once per episode, battery_reading_unavailable, with `id`, `sequence`, `active`; the low / critical percents follow the saved `/v1/battery-settings` low_percent / warning_percent; `shutdown_imminent` is no longer sent), plus 2.2 event notifications such as `ac_output_limit` / `ac_output_shutdown` (DSP) |
| `POST /state` | token | `{"state":"on"\|"off"}` → `{ok,state,applied_via:"stm32_set_mode"\|"pwr_key"}`; applied from `loop()` |
| `POST /relay` | token | `{"socket_id":"...","state":"on"\|"off"}` → 202 |
| `POST /register` / `/unregister` | token | `{"socket_id":"..."}` (also `id`). `/unregister` also clears the saved socket name, pushes SSE `state` at once and logs `SOCK_REMOVE_OK` |
| `POST /ota` | token (not the setup credential) | Local OTA over the hotspot or LAN. Body: the app image (`firmware.bin`), raw `application/octet-stream` or multipart. Required header `X-Firmware-SHA256` (64 hex). The image must carry `POWERCELL_FW\|collector\|<this model>\|<version>\|END`; same version refused, older only with `?force=1`. Refused (409) while battery-latched, during a cloud OTA or while the running image is unconfirmed. 200 `{ok,from,model,pending_verify,reboot_in_ms}` then reboot. Errors: 400 `x_firmware_sha256_required`/`not_esp32_image`/`image_too_small`, 409 `same_version`/`downgrade_refused`/`battery_critical`/`cloud_ota_busy`/`current_image_not_confirmed`/`ota_busy`, 413, 422 `sha256_mismatch`/`identity_or_model_mismatch`. Tool: `tools/ota_upload.py firmware.bin --model <model>` |
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

## Factory (one command per unit)
`tools/factory.py` does the whole factory step over USB (pyserial, 115200). Setup once on the factory PC:
copy `tools/factory_state/factory.env.example` to `tools/factory_state/factory.env` and fill `API_KEY`
(and `ADMIN_KEY` for `--register`). Leave `OTA_URL` and `API_URL` blank to use
`https://api-dev.acecore.tech`, which is also the firmware default (`POWERCELL_OTA_BASE_URL_DEFAULT`
and the WebSocket host). Fill both with that same URL if you set them by hand. A blank value is not
sent as `SET ota_url`. Everything in `tools/factory_state/` except the example is gitignored.
```
pip install pyserial
python tools\factory.py COM11                     # bench unit
python tools\factory.py COM11 --register --lock   # first customer units (plus secure boot)
python tools\factory.py COM11 --dry-run           # read-only plan
```
- Serial `ACE-NNNNNN` from `factory_state/serial_counter.json` (`{"prefix":"ACE-","next":1}`); bumped only after
  `OK serial set`; also checked against `units.csv` so a serial is never assigned twice. A unit that already has a
  serial is refused unless `--reuse`. `LOCK` runs only after registration succeeds. A failure at `SET ota_url`
  (or any earlier `SET`) does not write a serial, does not bump the counter, and does not lock the unit.
  Run the same command again. A fresh counter still assigns `ACE-000001`. If `INFO` already shows a serial,
  rerun with `--reuse`.
- The device secret and hotspot password are generated on the board (`ROTATE_SECRET` only if missing). Secrets are never printed.
- `gw_name` is not set: with a serial the firmware uses the serial as gateway name and `Powercell-NNNNNN` as SSID.
- `--register` reads `hardware_model=` from `INFO` (the image's `POWERCELL_HARDWARE_MODEL`). It refuses to register
  if that line is missing or is not `pc-dsp011-5kw`, `pc-dsp011-35kw`, `pc-stm01-5kw`, or `pc-stm01-35kw`.
  It then reads `SECRET` once and POSTs this body to `POST /api/devices/register` (header `X-Admin-Key`):
  `{device_id, device_secret, mac, gateway_id, firmware_version, device_kind, model}`.
  `model` is that hardware model. `--model PC5K` / `--model PC3_5K` is a different setting: it only runs `SET model` for the NVS unit model.
  The response's `local_tok` is then stored. Without `--register` the unit is not in the backend; `LOCK` is refused
  by firmware until the secret is reported, so `--lock` requires `--register`.
- Output: the label line (`PWC2;S=Powercell-NNNNNN;P=<hotspot pass>;D=ACE-NNNNNN`) printed and saved to
  `factory_state/labels/<serial>.txt` (with `hardware_model=` and the `LABEL_QR` Wi-Fi QR string); a row in
  `factory_state/units.csv`, including `hardware_model`.

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
`OTA_CONFIG {json}` from 1.0.x is still accepted (`token` is ignored). `FACTORY_RESET` (typed twice) wipes everything.

## Migration notes (1.0.x → 2.0 by OTA)
- **Secrets:** on first boot, 2.0 copies the 1.0.x NVS values into `pc_secure`:
  - `powercell_ota/api_key` and `url` (the 1.0.x 64-hex OTA `token` is no longer used: PR #3 OTA takes `API_KEY` as Bearer)
  - `wifi/ssid` and `pass`

  So units keep their backend identity with no USB visit.
- **ESP-NOW keys are NOT migrated.** They were compiled into 1.0.x. Before shipping 2.0 by OTA, either provision `espnow_keys` over USB, or add a one-off migration build that writes them to NVS (see the open questions below). Without them, sockets show as offline.
- **Gateway name:** it changes from `ACE4447` / `ACE9678` to `ACE-XXXXXX` unless you `SET gw_name ACE9678` on that unit. The backend should accept MAC-based gateway targets, which 2.0 accepts.
- **Network:** the static IP from 1.0.x is replaced by DHCP. The setup AP SSID changes to `Powercell-XXXXXX`.
- **Not migrated:** the old `energy_wh`, and the 2.0.0 ESP energy counter (`pc_energy` NVS) is no longer used; the old offline queue (JSON in `offline_sync/events`).
- **Version:** the four product images report `2.5.20`. Publish each under kind `collector` and its own model (`docs/OTA_HARDWARE_MODEL.md`).
- **STM32 link:** the STM32 still sends 9600 text, so the default `legacy_text` works with today's STM32 firmware. Switch both sides to framed together.

## Telemetry fields (WS `telemetry`, additive to 1.0.x)
- `battery`: `voltage` (BMS, else STM32, else IO34), `soc`, `source` (`bms`\|`stm32`\|`adc`\|`none`), `stm32_voltage`, `voltage_mismatch`, `bms_online`, `bms_age_ms`, `current` (A, + charging), `power` (W), `temperature`, `temperatures[2]`, `mos_temperature`, `cycles`, `soh`, `remaining_ah`, `full_ah`, `status` (`standby`\|`charging`\|`discharging`), `charge_mos`, `discharge_mos`, `balancing`, `cells[]` (V), `cell_min_v`, `cell_max_v`, `cell_diff_mv`, `alarm_bits`, `alarms[]` (names). BMS fields are `null` (arrays empty) when the BMS is offline.
- `inverter` (STM32 only): `stm32_online`, `requested_on`, `output_voltage`, `output_current`, `output_power` (V x I), `grid_voltage`, `grid_available`, `temperature`, `battery_voltage` (PA3), `system`, `switch`, `charge_state`, `fault_code`, `faults[]`, plus the framed-only extras.
- `solar`: `online`, `voltage`, `current`, `power` (null when the ADS1115 is silent).
- `system.peripherals`: `ads1115`, `lora`, `sd_card`, `sd_size_mb`.
- **Removed:** `inverter.pzem_ok`, `inverter.energy_kwh`, `inverter.frequency`, `inverter.power_factor`, `inverter.output_energy_wh`, `inverter.energy_source`; `/metrics` `pzem_ok`, `inverterFrequency`, `inverterPf`, `inverter_energy`; `/api/state` `energy_output_wh`.
- `/metrics` adds `battery_voltage_stm32`, `bms_online`, `battery_current`, `battery_power`, `battery_temperature` (now real), `bms_mos_temperature`, `battery_cycles`, `bms_alarm_bits`, `sd_card`; `lora_ready` now means the LoRa radio answered.

## App live fields (2.5.4)

Same flat keys in four places: `metrics.*` of GET /data and the SSE `state` event,
`live.*` of WS `telemetry`, and `live.*` of GET /v2/status. Unknown = `null`, never a fake 0.

| key | unit | source |
|---|---|---|
| `source` | battery\|grid\|solar\|none | STM32 SYS + PV power (>= 20 W) + BMS current sign. Grid presence (DSP debounced GPIO39; STM grid_v >= 160 V) gives `grid` when SYS is the DSP placeholder or unknown and the pack is not discharging (> -0.5 A) |
| `status` | inverting\|idle\|charging\|fault\|standby\|unknown | STM32 SYS/CHR/fault, BMS current > 0.5 A; `unknown` = no STM32 data in 5 s (DSP: no PZEM sample). With the grid override: `charging` or `idle` (pass-through) |
| `output_voltage`, `output_current` | V, A | STM32 `VOUT`/`IOUT` (sent only while SYS=INVERTER) |
| `output_power_w` | W (VA) | inverting: VOUT x IOUT; if IOUT missing, abs(BMS W) x 0.9 (`output_power_source` = `bms_estimate`); output off: 0 (`off`); grid pass-through: null (STM32 has no AC load meter in GRID mode) |
| `load_pct` | % | output_power_w / (usable_kw x 1000) |
| `inverter_temperature` | °C | STM32 `TEMP` (the STM32 clamps it to 25.0 at/below its 25 °C NTC point) |
| `model`, `rated_kva`, `usable_kw`, `rated_kw` | | NVS model (`SET model PC5K`); `rated_kw` = `usable_kw` |
| `battery_current_a`, `battery_power_w` | A, W | JK BMS, + charging, - discharging |
| `remaining_ah`, `full_capacity_ah`, `cycle_count`, `soh` | Ah, Ah, n, % | JK BMS |
| `time_to_empty_min` | min | remaining_ah / smoothed discharge current (EMA tau 60 s); null unless discharging > 0.3 A; cap 5940 |
| `time_to_full_min` | min | (full - remaining) / smoothed charge current; null unless charging > 0.3 A |

Models (ASSUMPTION: usable = 90 % of kVA, confirm with hardware): PC3K 3 kVA / 2.7 kW,
PC3_5K 3.5 kVA / 3.15 kW (Powercell Prime; same usable as the 35kw builds), PC5K 5 kVA /
4.5 kW, PC10K 10 kVA / 9.0 kW. Unset by default. Set with `SET model PC3_5K` or
`SET model PC5K` (USB console, allowed after LOCK, applies at once, `SET model none`
clears) or `tools/factory.py COMx --model PC3_5K`. Aliases `3.5K`, `PC3.5K`, `35K`,
`PC35K` normalize to `PC3_5K`. Also in INFO, SELFTEST and GET /v2/setup/info
(`powercell_model`, `rated_kva`, `usable_kw`; the old `model` key is still the board id /
OTA hardware model).

Solar: `solar.valid` / `metrics.solar_valid` is false and the PV values are null when the
ADS1115 has no reading in the last 5 s. On I2C errors the reader backs off (3 failed
cycles, then 1 s doubling to 30 s), logs one `SOLAR I2C_FAIL` WARN and one `SOLAR RECOVERED`
INFO; each transfer is bounded by a 20 ms Wire timeout and runs in its own task.

## Status
| Area | Status |
|---|---|
| Builds (esp32dev_stm01_5kw, esp32dev_stm01_35kw, esp32dev_dsp011_5kw, esp32dev_dsp011_35kw) | Product images. Live host api-dev.acecore.tech |
| Host tests | Done (154 checks) |
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
