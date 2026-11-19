# Collector firmware changelog

## AcePlug ESP-NOW socket 1.1.0-espnow-event (Caleb 19 Nov 2026 16:19 WAT) - EVENT/DEMO BUILD ONLY

**EVENT/DEMO ONLY. Not for customer units. Keys must rotate before customer builds.** New project `aceplug-espnow-firmware/`, the plug image that pairs with collector 2.5.40 using the same existing reference ESP-NOW keys (compiled in from the local, never-committed `include/espnow_pairing.h`). Build only, not tested on a unit.

- Base: reference socket 1.0.1 (Col PL.zip). Wire format, signing and pairing from the shared `pc_core/espnow_wire.h`, the header in 2.5.40.
- 10 A trip (15 s auto reclose), 8 A warning (clear 7 A), 3 trips in 5 min lockout (NVS, BOOT button 3 s to clear), 0.70 A noise removal and 0.15 A floor, ON refused while tripped / locked / OTA: ported from the LoRa AcePlug 1.0.3 (537d7f0 lineage). Replaces socket 1.0.1's 50 A trip.
- Trip and lockout reach 2.5.40 as the v1 overcurrent flag; the 8 A warning goes in an optional Status packet 2.5.40 ignores.
- AcePlug v0.1 CAD pins: relay IO15, LED IO4, CT IO35, BOOT IO0; RFM95 NSS IO5 held high. No conflicts.
- Host test `test_aceplug_espnow_110`: plug-signed packets verify with the collector decode and back, thresholds 9.99/10.0 A and 8.0/7.0 A hysteresis, noise, lockout, ini and source wiring.

## 2.5.40 (ESP-NOW smart sockets on, LoRa off, Caleb 19 Nov 2026 14:31 WAT) - EVENT/DEMO BUILD ONLY

**EVENT/DEMO ONLY. Not for customer units.** Caleb (19 Nov 2026 14:43 WAT): this build uses the existing ESP-NOW keys from his reference so units pair with the existing socket firmware. The keys are compiled into the bins (`POWERCELL_ESPNOW_EVENT_KEYS=1`, from the local, never-committed `include/espnow_pairing.h`; NVS `espnow_keys` overrides them). They must be rotated, and the flag dropped, before any customer build.

On top of 2.5.39 (6ae05ef), branch `cursor/espnow-socket-2540`. Six product images. Build only, not tested on a unit. Reference: Caleb's "Col PL.zip" (collector 1.0.4 + socket 1.0.1 ESP-NOW tree, close-range validated 4 Oct 2026). See `docs/ESPNOW_SOCKETS.md`.

- **Socket transport is ESP-NOW on all six images.** `platformio.ini` `${common.socket_link}` = `FEATURE_LORA=0 FEATURE_ESPNOW_SOCKETS=1` in every env. The LoRa code (`sockets_lora.cpp`, `lora_link.cpp`, `lora_socket_proto`) stays in the tree, compiled out; `${common.socket_link_lora}` switches an env back.
- **Wire format = collector 1.0.4 / socket 1.0.1**, byte for byte: 44-byte v1 packet, HMAC-SHA256 (16 bytes) with the fleet AUTH key, unicast frames ESP-NOW encrypted (PMK/LMK), broadcast beacon every 1 s, socket discover every 1 s, explicit pair (PairAck) and durable forget (ForgetAck). Now from `pc_core/espnow_wire.h` (the parked 907b9b4 header, host tested with a known-answer HMAC, tamper, field and sequence cases), no mbedtls dependency in the protocol.
- **Keys.** NVS `espnow_keys` (`SET espnow_keys <96 hex>`, PMK + LMK + AUTH, over USB before LOCK) wins. Event/demo images fall back to the built-in reference keys (`[ESPNOW][EVENT_KEYS]` in the boot log, values never logged). No key header is committed or zipped (`include/espnow_pairing.h` gitignored; `include/espnow_pairing.h.example` is a placeholder that fails the build if used as is). Without any keys the boot log shows `[ESPNOW][NO_KEYS]` and `[ESPNOW][SOCKETS_OFF]` and sockets stay off. `factory.py` / `factory_flash.py` / `provision.py` accept an optional `ESPNOW_KEYS=` in factory.env (validated, never printed); `factory.env.example` ships it blank.
- **Same app / cloud behaviour as the LoRa path:** add (pair: socket must be heard in the last 10 s and advertising "available to pair", then confirms), remove (hidden at once, durable release until the socket acknowledges), rename / schedules / shed rules (socket_cfg, unchanged), on/off with `cmd_id`, `desired`, `pending`, supersede by a new tap, confirmed from telemetry when the ack is missed, `timeout` after 6 tries 750 ms apart, change push to SSE, VA = collector V x socket I with the 0.2 A floor. `transport` reads `espnow`; `rssi`, `snr`, `firmware_version` are null (not carried by ESP-NOW v1); reset-lockout and set-limit answer unsupported (socket 1.0.1 has neither).
- **Wi-Fi coexistence.** ESP-NOW uses the station interface on the collector's current channel (router channel when connected, hotspot channel otherwise; AP and STA always share one channel). The socket scans channels 1..13 for the beacon, so no channel pinning and router channel changes are followed. Wi-Fi power save is off while ESP-NOW runs (`WIFI_PS_NONE`; modem sleep drops frames), modem sleep only in the 10 % battery low-power latch. Paused during a local OTA upload; re-initialised if Wi-Fi stops.
- **Encrypted-peer budget** (ESP-IDF limit 7): paired and pairing/forgetting sockets are never evicted; the stalest unpaired observation is. Device map bounded at 32.
- **SX1278 left fitted and idle:** its CS (IO4) is still driven HIGH on DSP, so it cannot answer SD traffic on the shared bus. `lora_ready` reports false.
- Unchanged: JK-PB active, JBD compiled out, socket rules / schedules / shedding on DSP and STM, GPIO15 behaviour, factory_flash tooling (plus ESPNOW_KEYS), api-dev backend.
- Host test `test_espnow_2540`; `check_dsp_bms` compiles with the 2.5.40 radio flags.

## 2.5.39 (socket rules on the DSP images, Caleb 19 Nov 2026 01:12 WAT)

On top of 2.5.38 (81317c1), branch `cursor/lora-socket-power-2536`. Six product images. Build only, not tested on a unit.

- **Per-socket rules pass on DSP.** `socketcfg::pass` (safety backup monitor, SoC shed, inverter overload, cloud override, Manual-mode schedules, Day/Night, SET_LIMIT retries, load_shed / mode_fallback events) now runs every 2 s on the three DSP images, the same engine and timing as STM. Socket schedules set from the app (Manual mode) now fire on DSP units.
- **DSP inputs** (`pc::sock::dspInputs`, host tested): SoC from the JK-PB BMS (fresh sample only, 0..100 %, else invalid and SoC shedding pauses as on STM); grid present from GPIO39 grid sense (unknown = down, as STM); latch = the 10 % battery latch (`outputLatched()`), not the boot hold, so sockets are not switched off at every boot before the first BMS frame; inverter overload = PZEM AC-output warning or AC-limit shutdown.
- **Low-power radio duty on DSP** as STM: while the 10 % battery latch is set, Wi-Fi max modem sleep and the LoRa listen duty cycle (`[PWR][LOW_POWER_ON]`, `[LORA][SLEEP]/[WAKE]`).
- **Schedule clock.** `pc::clk::scheduleClockValid`: NTP or phone time always; on DSP also the DS3231 RTC (read once at boot, accepted only when coherent: OSF clear, valid calendar, 2023..2100, and only ever written from NTP/phone time). The NVS last-good restore and no time never run a schedule. Local time uses the saved tz (default WAT +60).
- **No schedule action on invalid time.** With an invalid clock schedules pause (no on/off), and a running schedule is held: the inverse "window ended" action is not sent until the clock is valid again (`holdForInvalidClock`). Applies to STM too.
- New log line when a schedule decision changes: `[SOCK][SCHEDULE] socket=<id> want=on|off at HH:MM wday=N clock=ntp|phone|rtc`.
- DSP `power_guard` JSON adds `low_power`, `soc`, `wifi_ps`, `priority_source`, `inverter_overload`, `grid_down`, `socket_rules_latched`, `shed_sockets`.
- Unchanged: GPIO15 inverter behaviour (the DSP socket block reads only, never calls gateway), socket trip 10 A / warn 8 A, JK-PB active, JBD compiled out, ESP-NOW off.
- Host test `test_dsp_socket_rules_2539`.

## 2.5.38 (LoRa on the DSP images, Caleb 18 Nov 2026 18:08 WAT)

On top of 2.5.37 (5dbba16) plus a merge of `cursor/factory-flash-tool` (0474688, tools only), branch `cursor/lora-socket-power-2536`. Six product images. Build only, not tested on a unit.

- **LoRa on DSP v0.1.1.** Caleb: every board, DSP included, has the SX1278 LoRa module fitted. The three DSP envs now build with `FEATURE_LORA=1` (so `FEATURE_LORA_SOCKETS=1`); the `#error "hardware v0.1.1 is not fitted with LoRa"` in config.h is gone.
- **Pins (no remap).** Same LoRa header pins as the STM boards: shared SPI SCK 18 / MISO 19 / MOSI 23, LoRa CS IO4, RESET IO26, DIO0 not wired (polled RX). The DSP SD card keeps its own CS IO5 on the same bus. New compile-time checks in pins.h: LoRa CS and RESET are not any DSP pin (BMS 17/16, PZEM 35/32, GPIO15 inverter enable, LEDs 13/12/14, front light 27, NTC 33, fan 25, grid sense 39, I2C 21/22, SPI 18/19/23, SD CS 5). IO4 and IO26 are not used by anything else in the DSP firmware. Not checked against a DSP v0.1.1 schematic: that the DSP board routes the LoRa header to IO4 / IO26 is taken from the collector header map.
- **Shared SPI bus.** periph::begin() drives SD CS (IO5) and LoRa CS (IO4) HIGH before `SPI.begin(18, 19, 23)`, then brings up LoRa under the SPI mutex. The DSP SD card is mounted later by hwLogStart() under the same mutex, and every LoRa and SD access takes that mutex.
- **Sockets on DSP.** `sockets[]` in telemetry, `/metrics`, `/sockets`, `/data` and console `SOCKETS` now list paired LoRa sockets. Socket voltage and VA use the PZEM-004T v3 AC output voltage (`stm32link::status()` on DSP), floor 0.2 A as 2.5.37. Cloud socket commands and local `/relay` are no longer rejected with `lora_not_fitted` on DSP.
- **Serial monitor.** On boot a DSP unit now prints `[LORA] INIT 433 MHz SF9 cs=4 rst=26 (polling)` and `[LORA] SOCKETS_READY 433 MHz sync 0xF1 polled RX`, or `[LORA] INIT SX127x not answering` and `[LORA] SOCKETS_OFF ...` if the radio does not answer.
- **Not on DSP yet:** the per-socket rules pass (`socketcfg::pass`: SoC shed, schedules, cloud override, low-power radio duty) still runs only in the STM power_guard path. DSP sockets read and switch, and the socket's own 10 A trip / 8 A warn apply.
- Unchanged: JK-PB on all six images (JBD compiled out), ESP-NOW off, LoRa packet format, socket firmware.
- Host test `test_dsp_lora_2538`; `check_dsp_bms` compiles with `FEATURE_LORA=1` like the images.

## 2.5.37 (socket noise floor 0.2 A, Caleb 18 Nov 2026 17:02 WAT)

On top of 2.5.36 (1529379), branch `cursor/lora-socket-power-2536`. Six product images. Build only, not tested on a unit. New version number because 2.5.36 was already built and delivered with the 0.1 A floor, and OTA compares versions.

- Socket power noise floor raised from 0.1 A to 0.2 A (`pc::sockpower::kNoiseFloorA`): below 0.2 A `apparent_power_va` and `current_usage_watts` are 0 VA, so the idle CT reading (about 0.15 A) no longer shows about 35 VA with nothing plugged in. `current` is still reported as measured. The smallest non-zero power is now about 46 VA at 230 V.
- Everything else as 2.5.36: JK-PB on all six images, voltage window 10 s, warn/trip amps based (10 A / 8 A), LoRa packet format, socket firmware and ESP-NOW (off) unchanged.
- Host test `test_socket_power_2536` checks the 0.2 A floor (0.15 A and 0.199 A give 0 VA, 0.2 A gives 46 VA at 230 V).

## 2.5.36 (LoRa smart-socket power, V x I, Caleb 18 Nov 2026)

On top of 2.5.35 (ad90eb0), branch `cursor/lora-socket-power-2536`. Six product images. Build only, not tested on a unit.

**BMS back to JK-PB (Caleb 18 Nov 2026 16:51 WAT).** All six envs select JK-PB exactly as 2.5.33: no `${common.bms_jbd}`, DSP envs `DSP_BMS_JKPB=1`, JK-PB 115200 8N1 listen-only on Serial2 ESP RX GPIO17 / TX GPIO16 (STM and DSP). The env build flags are the 2.5.33 flags with only the version changed. The 2.5.35 JBD poller and its fixes are kept intact and compiled out; select it again with `${common.bms_jbd}` (and `DSP_BMS_JKPB=0` on DSP).
- JK images behave as 2.5.33: `pc::jk::Data` defaults `tempCount` 2, `hasMosTemp` and `hasSoh` true, so `soh`, `mos_temperature`, `bms_mos_temperature`, `temperature`/`temperatures` are published as before (only the JBD path nulls them). JK alarm bits 0..23 map to the same events; bits 24..26 stay unnamed in JK images. Console INFO prints `bms=JK-PB baud=115200 rx=17 tx=16` (2.5.35 also prints it on STM images, 2.5.33 only on DSP). Small 2.5.35 carry-overs that also apply to JK: `cell_min_v`/`cell_max_v` are null instead of 65.535/0 when no cell list has arrived, and SELFTEST adds `alarm_bits`.
- `test_dsp_bms_select` now requires JK-PB in all six envs (no `${common.bms_jbd}`, no `POWERCELL_BMS_JBD`, DSP `DSP_BMS_JKPB=1`) and checks the JK defaults and alarm names. `jbd_host_tests` and the `check_dsp_bms` compile checks still cover the JBD option.

**LoRa smart-socket power.** Collector side only: the LoRa packet format, the socket firmware and ESP-NOW (`FEATURE_ESPNOW_SOCKETS` stays 0) are unchanged.

- Every paired socket in `populateDeviceArray()` (so WS telemetry `sockets[]`, `/metrics`, `/sockets`, `/data`, console `SOCKETS`) gains `voltage`, `voltage_source` (`"collector_output"`), `apparent_power_va`, `power_quantity` (`"apparent_va"`) and `current_usage_watts` (same number as `apparent_power_va`, the key the app and backend already read).
- Power = collector AC output voltage x socket current. Voltage: STM, the STM32 status `vout`; DSP, the PZEM-004T v3 AC voltage (`stm32link::status()`). The sample must be fresh: no older than 10 s (`pc::sockpower::kVoltageFreshMs`), and the existing rules still apply (STM32 status under `STM32_STALE_MS` = 6 s, PZEM sample under 2 s). Stale, missing or implausible (outside 0..300 V) voltage: `voltage`, `apparent_power_va` and `current_usage_watts` are null. An offline socket keeps `voltage` but its power is null. 0 V (inverter output off) is a real reading and gives 0 VA.
- Noise floor 0.1 A (raised to 0.2 A in 2.5.37): below it the power is 0 VA. `current` is still reported as measured (the CT reads about 0.15 A idle on some units, so expect up to about 35 VA with nothing plugged in; raise the floor if Caleb sees that on a unit).
- **V x I is apparent power (VA), not real watts.** It matches real watts only for resistive loads (kettle, iron, heater, bulb). For motors, fridges, fans, phone and laptop chargers and other electronics it overstates the real watts (real W = VA x power factor). Labelled as such in the data.
- Warnings and trips are unchanged and stay current based (amps): socket 10 A trip / 8 A warn, socket config limits, `socket_warn_8a` / `socket_tripped_10a`.
- The compact `/data` snapshot copy (used when the 4 KB snapshot drops `metrics.sockets`) keeps the five power keys too (`pc::sockpower::kCompactSnapshotKeys`).
- The offline queue `socket_state` entry carries only the socket id and the requested state, no readings, so it is unchanged.
- DSP images have no LoRa (stub transport, no paired sockets), so their `sockets[]` stays empty; the stub is unchanged.
- Host test `test_socket_power_2536`: math, noise floor, stale/implausible voltage, offline socket, JSON keys, compact snapshot keys, voltage read before `dataMutex`, ESP-NOW still off.

## 2.5.35 (JBD BMS on every image, Caleb 18 Nov 2026)

On top of 2.5.33 (25223cf), branch `cursor/jbd-bms-2535`. 2.5.34 (b3f1e0a, GPIO15 momentary power-key press) is NOT in this build; Caleb set the power-button change aside. 2.5.34 stays reserved for that build. Six product images. Build only, not tested on a unit.

- BMS driver selection is one flag, `POWERCELL_BMS_JBD` (`config.h`). `platformio.ini` `[common] bms_jbd = -D POWERCELL_BMS_JBD=1` is in all six envs; the DSP envs set `DSP_BMS_JKPB=0`. `config.h` refuses a DSP build where `DSP_BMS_JKPB` and `POWERCELL_BMS_JBD` agree. JK-PB code is unchanged and selectable: drop `${common.bms_jbd}` (and set `DSP_BMS_JKPB=1` on DSP).
- Pins and baud: Serial2 ESP RX GPIO17 / TX GPIO16 on every board (the connector Dubem's working firmware and the JK-PB used), `JBD_BMS_BAUD` 9600 8N1. The old JBD map RX16/TX17 (read nothing on the DSP board) is gone; `DSP_BMS_UART_SWAP=1` still swaps the pair. INFO now prints `bms=JBD baud=9600 rx=17 tx=16` on STM images too.
- Poller (`src/bms.cpp`): `DD A5 03 00 FF FD 77` basic info, then `DD A5 04 00 FF FC 77` cells, every `JBD_POLL_MS` (2000 ms). Replies are checked for start/end, register, length, checksum and status byte (0x80 = refused). Timeouts, checksum errors and bad frames are counted in the BMS stats (`frames`, `ok`, `timeouts`, `checksum_errors`, `bad_frames`) with rate-limited `[BMS][JBD]` warnings. A failed poll publishes nothing; the last good reading goes stale after 10 s (`JK_BMS_STALE_MS`) and every BMS field becomes null, as on the JK path. The `BMS RAW` console sends one read-only 0x03 request (the JBD only talks when asked) and the poller now pauses for it.
- Protection word -> JK alarm bit layout (`jbd::toJkAlarms`), so the shared event map works: cell OV -> bms_4, cell UV -> bms_11 (battery_low), pack OV -> bms_5, pack UV -> bms_12 (battery_low), charge OT -> bms_8 and discharge OT -> bms_15 (both `bms_over_temp`, never the inverter `over_temp`), charge UT -> bms_9, charge OC -> bms_6, discharge OC -> bms_13, short circuit -> bms_14 (overcurrent). JBD-only bits are named only in JBD images: discharge UT -> bms_24 `discharge_undertemp`, front-end IC error -> bms_25 `afe_ic_error` (critical bms_fault), MOS software lock -> bms_26 `mos_software_lock`. Before 2.5.35 the raw JBD word was passed as JK bits (cell UV would have shown as JK `mos_overtemp`).
- Data (`jbd::toJkData`): pack V, current (+ charge, - discharge, 0.2 A status deadband), power, SOC (same SOC-vs-capacity plausibility rule as JK), remaining/full Ah, cycles, MOS states, balancing, cells (0x04). NTCs: the count from the frame, up to 8 parsed, first 2 published; an out-of-range NTC (-40..120 C) is dropped. If the 0x04 read fails, the previous same-count cell list is kept, else no cells are published (never 0 V cells). JBD has no MOS temperature or SOH: `mos_temperature`, `bms_mos_temperature`, `soh` are null instead of 0, `temperatures` lists only real sensors, and `cell_min_v`/`cell_max_v` are null without a cell list. JK images publish exactly as before (`tempCount` 2, `hasMosTemp`, `hasSoh` default true).
- STM boards: the ESP reads the BMS itself on Serial2 through the RS485 header (the STM32 does not read the BMS), so the STM images poll the JBD too. This needs an RS485-capable JBD (or a TTL JBD wired straight to RX17/TX16 without the RS485 module) and the module's auto-direction to pass the ESP's TX.
- Tests: `test_jbd` extended (published 15S 0x03 reply with checksum FB FF, checksum/status/truncation, current sign, NTC count and open sensor, protection map, Data conversion, events). New `jbd_host_tests` (`test_jbd_image.cpp`, built with `POWERCELL_BMS_JBD=1`): every JBD protection bit raises its one bms_N event with the right type and severity and clears after 5 s. `check_dsp_bms` builds JBD for every DSP rating and STM, keeps JK-PB, and proves the conflicting-flag build fails. `test_dsp_bms_select` checks all six envs.

## 2.5.33 (new model: Powercell Prime 1.5 kVA / 3 kWh, Caleb 16 Nov 2026)

On top of 2.5.32 (3c60b00). Six product images (four existing plus two new). Build only.

**PROVISIONAL:** every 1.5 kVA rating below is scaled from the 3.5 kVA model (usable = rated x 3150 / 3500 = 90 %) until Caleb sends the real 1.5 kVA hardware specs. Revisit before shipping a 1.5 kVA unit.

- New OTA hardware models `pc-dsp011-15kw` and `pc-stm01-15kw` (Caleb 16 Nov), envs `esp32dev_dsp011_15kw` and `esp32dev_stm01_15kw`. Each mirrors its board's 35kw env: same pins, same JK-PB BMS (115200, RX17/TX16 on DSP), same feature flags. Customer name "Prime" (`productNameForHardwareModel`).
- DSP factory rating `POWERCELL_FACTORY_RATED_ACTIVE_W=1500`, `POWERCELL_FACTORY_VARIANT="pc-dsp011-15kw"`; `SystemRating::kRated15kW = 1500`, `kUsable15kW = 1350` (3.5 kVA: 3500 / 3150). `factory_rating.h` now accepts 0, 5000, 3500 or 1500. The AC ceiling maximum and the system overload bound follow the usable value (1350 W). `pins.h` asserts JK-PB on the 1500 W DSP image. The STM 15kw image carries no factory rating, like the other STM images.
- New NVS model `PC1_5K` (1.5 kVA / 1.35 kW usable) in `pc::live` models, with aliases `1.5K`, `PC1.5K`, `1.5KVA`, `15K`, `PC15K`. The legacy `PC3K` (3 kVA) is unchanged and is not reused.
- `tools/factory.py`: `OTA_MODELS` and NVS `MODELS` include the new ids; `normalize_model` aliases; mismatch warnings for PC1_5K vs 5kw/35kw images and for PC5K/PC3_5K/PC3K/PC10K on 15kw images. `tools/ota_upload.py` accepts the two new models.
- Not scaled (not keyed by model today): the STM32 ESP-side overload rule (19 A raise / 17 A clear, `events_core.h`) and the STM energy limits (32 A, 280 V). Both are shared by the 5 kW and 3.5 kVA STM images. Battery capacity is not configured in firmware (it is read from the BMS), so 3 kWh appears only in docs.
- Docs: `FACTORY_RATING.md`, `OTA_HARDWARE_MODEL.md`, README, `LOCAL_MANUAL_CONTRACT.md`, `POWER_UNITS.md`, `WIFI_AP_FALLBACK.md`, `DSP_HW_CONFLICTS.md`.
- Backend and app must learn the two new ids before a 1.5 kVA unit can register or get OTA (see `docs/OTA_HARDWARE_MODEL.md`).
- Host test `test_prime_15_2533`; `test_ota_hardware_model`, `test_factory_rating`, `test_dsp_bms_select` and `test_product_names_2532` extended; Makefile compile check `check_dsp_bms_15kw`; factory tests class `Prime15`.

## 2.5.32 (product rename for the 20 Nov launch, Caleb 16 Nov 2026)

On top of 2.5.31 (b0d4cfd). All four product images. Display, factory-warning and documentation names only. Model IDs, env names, OTA identity strings and provisioning keys are unchanged.

- 3.5 kVA units (`pc-dsp011-35kw`, `pc-stm01-35kw`, NVS `PC3_5K`) are Powercell Prime Plus (was Prime).
- 5 kVA units (`pc-dsp011-5kw`, `pc-stm01-5kw`, NVS `PC5K`) are Powercell Ultra (was Prime Plus).
- No 1.5 kVA / 3 kWh (Powercell Prime) model id is added. Legacy `PC3K` (3 kVA) is unchanged and is not Prime.
- Usable-power figures are unchanged (3.15 kW / 4.5 kW). Battery sizes were not stated in these strings.
- There is still no `product_name` JSON field, and the new helper is not sent on the wire or used for OTA matching.
- Factory mismatch warnings, comments, README, `docs/OTA_HARDWARE_MODEL.md` and `docs/FACTORY_RATING.md` updated. Host test `test_product_names_2532`.


## 2.5.31 (false repeating over_temp / temp_sensor_fault alerts, ACE-000003 / ACE-000001, Caleb 15 Nov 2026)

On top of 2.5.30 (07a6810). All four product images. Prompted by ACE-000003 (DSP 5 kW) on 2.5.29/2.5.30: an over_temp critical alert every 60 s (at second :43) with the NTC valid at ~31 C, `over_temp_cutoff` false and nothing in `active_events`; the backend made 71 "Inverter too hot" + "Alert cleared" pairs since 23:03 WAT. ACE-000001 logged 102 temp_sensor_fault raises and 89 clears.

- **60 s replay (root cause):** `events.cpp` `record()` hands every event transition to the offline queue as `ev_<type>` while the WS is down. On ACE-000003 the WS never came up before 2.5.29 (TLS heap gate), so `ev_over_temp` records sat in the NVS `pc_offline` queue. Once the WS came up, `offlineq::service()` sent them in every `offline_sync` batch and, with no `offline_sync_ack` removing them, re-sent the batch on the doubling backoff that caps at 60 s (`offline_queue.cpp`, `backoff = backoff < 60000 ? backoff * 2 : 60000`), forever. The engine itself emits nothing in that state (host test: 2 h at 31 C, zero events). Fix (`offline_policy.h`): ev_* records are dropped after 3 `offline_sync` sends with no ack (`[SYNC][EV_DROPPED]`), ev_* records restored from NVS at boot are dropped (`[SYNC][STALE_EVENTS]`; live state goes in `active_events`), and an ack that matches no id no longer resets the count. User actions (`gateway_state`, `socket_state`, `socket_reset_lockout`) and `socket_*` lifecycle records keep retry-until-ack. `[SYNC][RESEND]` logs repeated sends.
- **Event `id` on the wire:** `eventToJson` (now `pc::ev::eventFieldsTo`, `events_json.h`, host tested) adds `"id"`: the stable condition id (`over_temperature`, `bms_1`, `bms_8`, `bms_15`, `bms_21`, `temp_sensor_fault`, `temp_warning`, `stm32_0x20`, ...). Applies to telemetry `events[]`, `active_events[]` and the notification `event` object.
- **BMS temperature alarms are not over_temp:** JK-PB alarm bits 1/8/15/21 now have type `bms_over_temp` (codes `mos_overtemp`, `charge_overtemp`, `discharge_overtemp`, `battery_overtemp`; ids `bms_1`/`bms_8`/`bms_15`/`bms_21`; severity critical, source `bms`). Offline action `ev_bms_over_temp`. Title "Battery protection: temperature too high". `over_temp` is now only the DSP NTC cutoff latch (`over_temperature`) and the STM32 thermal alerts (`stm32_0x20`, `temp_warning`).
- **temp_sensor_fault anti-flap:** raised after 10 s of continuous fault (was 4 s), cleared only after 120 s of continuous valid readings (was 10 s); a new episode within 10 min of a clear needs 60 s of fault. An invalid NTC is temp_sensor_fault only (the latch needs a valid reading >= 80 C).
- **over_temp cutoff in active_events:** held active for as long as the latch is set (cleared only by the user ON below 70 C), including straight after boot from the NVS latch. The event `value` and new `temp_c` field carry the live NTC temperature when valid, else the trip temperature (`ot_c10`), else null.
- **thermal task stack** 3072 -> 4096 B (MEM TASKS showed 868 B free). async_tcp stack (16384, ~15.8 KB never used at idle) left unchanged: the local web handlers run on it and no on-unit high-water under HTTP load is available yet. TLS gate unchanged.
- Host test `test_false_alerts_2531`; `test_dsp_over_temp` updated for the new temp_sensor_fault timing; the BMS case in the events test expects `bms_over_temp`.

## 2.5.30 (DRAM headroom for the WS TLS session on the DSP board, ACE-000003, Caleb 14 Nov 2026)

On top of 2.5.29 (004948c). All four product images (the SD changes are DSP-only). ACE-000003 (DSP 5 kW) on 2.5.29 without an SD card: `TLS_UP free 69592->16256 largest 55284->4084 min=628`, then `heap_free=17592 largest=4084`; after a Wi-Fi drop `TLS_FAILED tls=-9984` twice at ~52 KB free; `DROP_REASON ... tls=48 PADLOCK - Input data should be aligned`; `MDNS_FAILED` every 30 s; `[SD][CARD] present=0 dram_used=117588`.

- **SD context returned when unused:** `SD.begin` returned true without a usable card (the success branch logged `present=0`), so the 14520 B FatFs context stayed allocated with no file open. Now both file objects are closed and `SD.end()` unmounts unless `/blackbox.bin` or `/acenergy.bin` opened. Without a card the `hwlog` task also ends and returns its 8192 B stack (it only drains to and observes the SD log). About 22.9 KB of DRAM back on a unit without a card. Unchanged with a working card.
- **SD log line:** the 2.5.29 `dram_used` compared free DRAM at task start with free DRAM after the mount (up to 60 s later), so it counted the whole boot. Now `[SD][CARD] cs=5 mount=1 blackbox=<0|1> energy=<0|1> size_mb=.. mount_ms=.. ctx=14520 held=<0|1> free=..` or `no card on GPIO5 mount=0 mount_ms=.. held=0 free=..`, and `[SD][TASK_END]` when the task stops.
- **Fallback hotspot closes sooner:** after a FALLBACK, when the router returns and no phone is on the hotspot, it closes after 10 s (`kApQuickGraceMs`) instead of 120 s, so the WS TLS reconnect does not run in AP+STA. The 120 s grace is unchanged after `/v2/wifi/save`, a network-mode change, or while a phone is associated; testing, forced (`AP ON`) and hotspot-only are unchanged.
- **mDNS heap rule:** mDNS (~8-12 KB with its task, PCBs and multicast RX) starts only with an 8 KB block and, with the WS up, 32 KB free; with the WS down, only if the TLS floor (51200) still holds after it (63488 free). When the WS is down and free DRAM is below the floor, a running mDNS is stopped so the reconnect gets that memory (`[WIFI][MDNS_OFF] released for the WS TLS reconnect`). Deferred starts log `[WIFI][MDNS_DEFERRED]` at most every 5 min (no 30 s MDNS.begin churn). On 2.5.29 mDNS restarted 0.5 s before the failed reconnect.
- **`tls=48 PADLOCK` was not an error:** `WiFiClientSecure::lastError()` returns `start_ssl_client()`'s value, which after a good connect is the socket fd (48), and `mbedtls_strerror(48)` printed the PADLOCK text. Only negative values are reported now (`DROP_REASON` and `TLS_FAILED`). The 54406 drop itself was the Wi-Fi link (5 s blocked ws pass, `WIFI DOWN` 1 s later).
- **`[MEM][TASKS]` / `[MEM][HOLD]`** every 5 min (first after 60 s): never-used stack bytes for loopTask, async_tcp, ws, hwlog, pzem, bms, grid, thermal, solar, mdns, ota, ota_boot, tiT, wifi (-1 = not running), then task count, ws_up, ap, mdns, sd, free, largest, min. For trimming stacks on evidence later; no stack size is changed here.
- TLS gate unchanged (51200 free, 18432/35520 blocks). The server sends a 4-certificate ECDSA chain (leaf <- YE1 <- Root YE <- ISRG Root X2 <- X1 cross-sign); mbedTLS parses all four and verifies up to X2 after both 16717 B record buffers are allocated, and an allocation failure in that ECDSA verify is reported as `-9984 X509 verify failed` (same signature as the v10 field case). The record buffer size is fixed in the precompiled mbedTLS.
- Host test `test_heap_headroom_2530` (quick grace, phone keeps grace, save keeps 120 s, engine default unchanged, mDNS rule with the ACE-000003 numbers, SD keep rule, fd vs error, source checks).

## 2.5.29 (SD mount DRAM: WS TLS never started on the real DSP board, ACE-000003, Caleb 14 Nov 2026)

On top of 2.5.28 (3427597). All four product images (the change is in the DSP SD path; STM images only get the version). Prompted by ACE-000003 (DSP 5 kW) on 2.5.28, on the board: `[MEM][HEAP_LOW] ws TLS deferred: low_free largest=32756 free=45432 min=48 need_free=51200`, while the same ESP32 off the board connects.

- **Cause:** the DSP log task mounts the SD card with `SD.begin(5, SPI, 1000000)`, i.e. the Arduino default `max_files = 5`. IDF 4.4 `esp_vfs_fat_register()` makes one DRAM allocation of `6248 + 5 x sizeof(FIL)` for the uptime; with 4096 B sectors and per-file cache `sizeof(FIL)` is 4136, so 26928 B. The firmware only ever has two files open (`/blackbox.bin`, `/acenergy.bin`). The bench ESP32 has no card, so its DRAM was ~27 KB higher and passed the 51200 B TLS floor; on the board free sat at ~45 KB and the WS never tried.
- **Fix:** mount with `max_files = 2` (`pc::kSdMaxFiles`): 14520 B, 12408 B of DRAM returned (45432 -> ~57840 expected, above the floor; the two-record-buffer probe then decides because largest < 35520). A compile-time check ties `sizeof(FIL)` to the documented 4136. The TLS gate (51200 free, 18432/35520 blocks) is unchanged: the handshake still needs both 16717 B record buffers (precompiled mbedTLS, 16384 content length, no variable buffers, so MFLN would not shrink them).
- **Log:** `[SD][CARD] cs=5 present=1 max_files=2 dram_used=<B> free=<B>` at boot.
- `heap_min=48` / `largest_min=500` is a transient low since boot that these lines do not identify (every TLS connect is behind the guard, which logs `TLS_UP`/`TLS_FAILED` with `min=`). No leak found in the PZEM no_reply or JK-PB parse paths (stack buffers only).
- Host test `test_sd_mount_dram` (context sizes, 12408 B saving, ACE-000003 numbers vs the gate, source checks).

## 2.5.28 (Wi-Fi scan in AP+STA fallback, ACE-000003, Caleb 14 Nov 2026)

On top of 2.5.27 (34a3b1d, = 2.5.26 7e6f960 code). All four product images. Prompted by ACE-000003 (DSP 5 kW): with the saved router off, the unit is in AP+STA fallback and `service()` called `WiFi.reconnect()` every 5 s (`kWifiRetryMs`) on top of the SDK auto-reconnect. That refused the async scan (`ESP_ERR_WIFI_STATE`) or aborted it (SCAN_DONE with 0 records), and the 0 was cached as fresh for 30 s, so `POST /v2/wifi/scan` answered `scanning:false` with an empty list.

- **Reconnect pause:** while a scan is requested or running, and 4 s (`kScanQuietMs`) after the last attempt, the 5 s `WiFi.reconnect()` loop is skipped and the SDK auto-reconnect is switched off; it is restored afterwards (off again in hotspot-only mode). A pending connect attempt for the lost router is dropped (`WiFi.disconnect(false,false)`, no SDK retry) right before a scan starts when the station is down. Logs `[WIFI][SCAN_PAUSE]` / `[WIFI][SCAN_RESUME]`.
- **No empty cache, retries:** only a non-empty result is cached for 30 s. A refused, timed out, aborted or empty scan is retried 1.5 s later, up to 3 attempts in total, and `/v2/wifi/scan` keeps `scanning:true` (with the previous list, if any) meanwhile. After 3 failed/empty attempts it answers `scanning:false` with `[]` for 5 s, then the next poll starts a new scan. Logs `[WIFI][SCAN_START] attempt=n/3`, `SCAN_DONE networks=n`, `SCAN_RETRY`, `SCAN_EMPTY`.
- **`/v2/wifi/test`:** the reconnect loop for the saved SSID stays off while a test is pending or running and while a successful test awaits `/v2/wifi/save` (now one tested gate, `pc::onb::stationRetryAllowed`). A scan pause never switches the SDK auto-reconnect off during a test (it retries the test network, not the saved one).
- Logic in `pc_core` (`ScanCtl`, `stationRetryAllowed`); host test `test_wifi_scan_ctl` (retry, no fresh empty cache, empty hold, quiet window, millis wrap, reconnect gate, source checks).

## 2.5.27 (OTA test of the 2.5.26 updater)

Version is 2.5.27 on all four product images so a unit already on 2.5.26 will accept the file. No other functional change on top of 7e6f960.

## 2.5.26 (stale boot OTA job: drop it instead of downloading another target, Caleb 13 Nov 2026)

On top of 2.5.25 (dd5e8a2, = 2.5.24 e814f46 code). All four product images. Prompted by ACE-000001 (DSP 5 kW): 2.5.22 had staged a 2.5.23 boot job (size 1393408) in NVS. The USB flash of 2.5.24 kept NVS, so 2.5.24 ran it at boot (`BOOT_JOB version=2.5.23 attempt=2/3`). Rollout 16 (2.5.23) had been cancelled and rollout 17 (2.5.25) opened; `/api/ota/download` serves whatever target is open, so the unit got the 1395536 B 2.5.25 file, stopped with `download_size_mismatch`, and the server failed the 2.5.25 target and completed the rollout.

- **Job stager:** every staged job stores the firmware that staged it (NVS `bj_stg` = version, `bj_bld` = first 16 hex of the ELF sha256). At boot, before Wi-Fi, a job is dropped when it was staged by another version or build (a USB flash or any image change since; jobs from 2.5.22..2.5.25 have no stager and are always dropped) or is for the running version.
- **Boot-time offer check:** in the `ota_boot` task, before the download, `POST /api/ota/check` (same TLS, CA and signed headers as the HTTPS check, on the early heap, one session freed before the download). No offer, or a different version or sha256: dropped. If the check fails, the job still runs when it is newer than the running image; an older job is dropped and its sha256 refused until `OTA_RETRY` (a downgrade runs only when the server confirms it, and cannot loop stage/drop reboots). Log `[OTA][BOOT_CHECK] job=.. check=offer|none|failed offer=.. verdict=..`. Boot run cap 9 minutes (was 6).
- **Download names the offer:** in place and on the boot path, `GET /api/ota/download?device_id=..&version=<v>&sha256=<hex>` (percent-encoded, not doubled if the server URL already has them). Older servers ignore them. 409/410 is `download_not_current` (was `download_http_409`).
- **Stale evidence at the download:** on the boot path, 409/410 or HTTP 200 with a `Content-Length` other than the job's size drops the job instead of failing it (`download_size_mismatch` stays for the in-place path and for missing/short lengths).
- **Drop reporting:** `[OTA][BOOT_JOB_DROPPED] reason=<staged_by_unknown|staged_by_other_build|same_version|not_newer|no_offer|offer_changed|not_current_offer|size_mismatch> job=<v> staged_by=<v|unknown> [offer=<v> | http= length= expect=] running=<v>`; the job is cleared and the attempt is not counted; `last_status: "dropped"`, `last_error: boot_ota_stale_job`, `last_error_detail` = the reason string. An in-place 409 reports `dropped` / `download_not_current` and does not fall back to the boot path. Never `failed`, so no rollout target is charged with it.
- **`last_version` in `ota_check`:** WS and HTTPS checks carry the version the last attempt was for, so the server can ignore a failure that is not about its open target (backend `cursor/ota-offer-guard`).
- Host tests: `test_ota_stale_job` (stager, same version, offer check, downgrade rule, download evidence, no re-stage loop, detail string, query encoding and bounds, source checks); `classifyResponse` 409/410.

## 2.5.25 (OTA test of the 2.5.24 updater)

Version is 2.5.25 on all four product images so a unit already on 2.5.24 will accept the file. No other functional change on top of e814f46.

## 2.5.24 (WS offer dropped when the OTA task stack does not fit: stage from the main loop, Caleb 12 Nov 2026)

On top of 2.5.23 (21299fc, = 2.5.22 00db613 code). All four product images. Prompted by ACE-000001 (DSP 5 kW) on 2.5.22: `[OTA][WS_AVAILABLE] version=2.5.23 size=1393408`, then nothing, with `LOOP STATS heap_free=35676 heap_largest=14836 largest_min=9204 iram_free=42844`. `service()` creates the 16384 B `ota` worker for a WS offer; with a 14836 B largest DRAM block `xTaskCreatePinnedToCore` failed and the offer was dropped without a log every check period, so the boot-path staging in `handleOffer()` never ran.

- **Task-create failures are logged:** `[OTA][TASK_CREATE_FAILED] task=ota for=ws_offer|https_check stack=16384 free_heap largest min iram`. The boot path's `ota_boot` task failure logs it too and `boot_ota_task_failed` now carries `stack=16384 heap free=.. largest=.. min=..`.
- **WS offer staged from the main loop:** when the `ota` task cannot be created, `stageFromLoop()` checks the offer with the same rules as the worker (`validateOffer()` -> `pc::bootota::checkOffer()`: version present, <= 32 chars and not the running one, sha256 hex, own-host download path with the `URL_FOREIGN` fallback, size <= next slot, sha256 not abandoned), sends `ota_status installing` (unless this sha256 is about to be abandoned), then `stageAndReboot()` with `last_error_detail` `in_place_gate=task_stack stack=16384 heap free=<B> largest=<B> min=<B>`, `boot_pending`, `BOOT_STAGED`, reboot. Same NVS job and 3 attempts per sha256. The WS is never released on this path, so a rejected (`JOB_REJECTED`, `TASK_STAGE_SKIPPED`) or abandoned offer cannot block reconnects. `handleOffer()` now uses `validateOffer()` too (identical rules and log lines; `JOB_REJECTED` for the version/sha/path case now starts with the reason name).
- **HTTPS check task failure** is logged; the check is retried at the next period (unchanged).
- **Local POST /ota:** no task is created there. `Update.begin()` failing for its 4 KB buffer (err=0) now returns 503 `update_begin_out_of_memory` (was 500 `update_begin_failed`), and the log carries the largest block.
- **Boot path unchanged:** `bootJob()` in `setup()` still runs the download in a 16384 B `ota_boot` task, created right after `net::begin()` and before ESP-NOW, the web server, the WS and mDNS, so it is allocated on the unfragmented early heap; `setup()` waits for it (6 min cap).
- Host tests: `test_ota_task_stage` (offer rules, stage/drop decision, attempt cap, detail string, source checks).

## 2.5.23 (OTA test of the 2.5.22 updater)

Version is 2.5.23 on all four product images so a unit already on 2.5.22 will accept the file. No other functional change on top of 00db613.

## 2.5.22 (OTA download TLS on a fragmented heap: two-phase OTA on boot, Caleb 12 Nov 2026)

On top of 2.5.21 (80c504e, = 2.5.20 898ffcc code). All four product images. Prompted by ACE-000001 (DSP 5 kW) on 2.5.20: the WS now connects (`TLS_UP attempt=1 ms=2388 free 131632->83216 largest 57332->40948`), but the download handshake right after the WS closed failed: `DOWNLOAD_TLS_START free_heap=101260 largest=40948 min=54792`, then `DOWNLOAD_TLS http=-1 tls=-32512 'SSL - Memory allocation failed'` after 235 ms, `FAILED download_connect_failed`.

- **Root cause, part 1 (the numbers were wrong):** every heap figure in the logs and the 2.5.20 gate came from `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()`, which use `MALLOC_CAP_INTERNAL` alone and so include the ESP32 IRAM heap (0x400A0000 minus the end of IRAM code: 43660 B on this image, one 40948 B free block after the allocator header). IRAM is 32-bit access only; mbedTLS (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`), Strings and JSON never use it, so the block never changes. That is why `largest=40948` appears in the 2.5.18 HEAP_LOW line, after the WS session in 2.5.20 and at the download start: it was never DRAM. Free was about 41 KB lower than logged (DRAM about 60 KB at the download start, and the boot minimum `heap_min=48820` was about 8 KB of DRAM). The gate saw largest 40948 >= 35520, skipped the real probe, and `mbedtls_ssl_setup()` could not get the two 16717 B record buffers from DRAM: -0x7F00 after 235 ms, before any ECC work.
- **Root cause, part 2 (why the WS close does not give it back):** the WS client and its mbedTLS ssl/config/CA/peer-chain contexts are freed on `ws.disconnect()` (WebSockets `clientDisconnect` stops and deletes the `WiFiClientSecure`; `stop_ssl_socket` frees everything; DRAM rose by about 35 KB, the size of one session). The CA bundle is parsed once per session, not twice. The memory comes back fragmented: while the WS was up the OTA worker's 16 KB stack (created when `ota_available` arrived), the first mDNS start (waits for the first WS connect) and JSON/String traffic were allocated around the session's buffers, so the freed pieces do not coalesce into two 16.7 KB blocks. A delay does not fix that.
- **MFLN / smaller record buffers (option c) do not apply:** the precompiled Arduino mbedTLS has `MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` but not `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` and no asymmetric or dynamic buffers, so the record buffers are 16717 B whatever is negotiated. Not used.
- **Two-phase OTA on boot (`lib/pc_core/src/ota_boot.h`):** after the WS is released for an install, the strict gate (now on 8-bit DRAM: free >= 50 KB and both record buffers fit, probed for real) decides. If it passes, the download runs in place as before. If it does not, or an in-place connect fails with an mbedTLS out-of-memory error, the job (version, download path, sha256, size, attempt count) is written to NVS `pc_ota` and the unit reboots. `setup()` runs the job right after Wi-Fi starts, before ESP-NOW, the web server, the WS and mDNS allocate: waits up to 40 s for the station IP and up to 30 s for NTP (signed request), then the same `install()`: same `WiFiClientSecure` with `POWERCELL_CA_CERT`, same sha256, identity and version checks, same error codes. Success reboots into the new image (rollback rules unchanged). Failure records `last_status: failed`, the error code and `last_error_detail`, and the boot continues normally; the server re-offers at the next check.
- **No boot loop:** the attempt is counted in NVS before it starts, so a crash, watchdog or power cut also counts. After 3 attempts of the same sha256 the job is abandoned: that sha256 is rejected before the WS is paused. A different sha256, a successful update or the new console command `OTA_RETRY` clears it. A staged job that did not run is cleared at the next boot (`boot_ota_not_run`); a job for the running version, an invalid one, or one found while the running image is still unconfirmed is discarded.
- **New status and codes:** `last_status: boot_pending` (staged, rebooting). `last_error`: `boot_ota_no_wifi` (no station IP within 40 s, or hotspot-only), `boot_ota_abandoned` (3 attempts started without a result), `boot_ota_not_run`, `boot_ota_task_failed`. All download codes are unchanged.
- **Heap numbers in the failure detail:** `last_error_detail` for a failed connect now ends with ` heap free=<B> largest=<B> min=<B>` (DRAM at the handshake start), and on the boot path ` boot=<n>/3` (plus ` abandoned`). The detail is up to 159 characters.
- **All heap logs are DRAM (8-bit) now:** `MEM HEAP_LOW`, `WS TLS_UP`/`TLS_FAILED`, `OTA CHECK`, `DOWNLOAD_TLS_START`/`_UP`/`DOWNLOAD_TLS`, `LOOP STATS`, console `INFO` and `device_hello` `heap_*`. Expect values about 41 KB lower than 2.5.20 printed for the same state. `LOOP STATS` adds `iram_free`, `device_hello` and `INFO` add `heap_iram_free`. New logs: `OTA HEAP_WS_RELEASED` (DRAM before and after the WS release and the gate verdict), `BOOT_STAGED`, `BOOT_JOB`, `BOOT_WIFI`, `BOOT_FAILED`, `BOOT_ABANDONED`. Telemetry `sys.free_heap` and the hotspot-only health check are unchanged.
- **WS guard:** same rule, now on DRAM numbers. On the 2.5.20 ACE-000001 log the first connect had about 90 KB DRAM free and a 57 KB block, so it still goes; a reconnect now probes the record buffers for real instead of trusting the IRAM block.
- Host tests for the boot decisions, attempt counting and abandon, staging, the out-of-memory classification, the detail strings, job validation and the setup order (1556 pass).
- **Upgrade note:** OTA from 2.5.20 or 2.5.21 still uses their old download path. 2.5.22 has to go on by USB first; OTA tests then run from 2.5.22 to a later version.

## 2.5.21 (OTA test of the 2.5.20 updater)

Version is 2.5.21 on all four product images so a unit already on 2.5.20 will accept the file. No other functional change on top of 898ffcc.

## 2.5.20 (WS and OTA TLS start on a fragmented heap, Caleb 12 Nov 2026)

On top of 2.5.19 (bbc5024). All four product images. Prompted by ACE-000001 (DSP 5 kW) on 2.5.18: `[MEM][HEAP_LOW] ws TLS deferred: largest=40948 < 40960 free=72064 min=47604`, repeating every 60 s, so no WS, no `ota_check`, no OTA.

- **Root cause:** the pre-TLS memory guard demanded one free block of 40960 B. 2.5.18 holds about 2.5 KB more static DRAM than 2.5.13 (mDNS adds about 1.9 KB of `.bss` inside the precompiled `libmdns`, plus the hotspot `Update` object). Static DRAM ends where the first heap region starts, so that region shrank by the same amount. On this unit the largest block ended up 12 B under the threshold, though a 40948 B block holds both mbedTLS record buffers.
- **New TLS guard** (`lib/pc_core/src/heap_guard.h`), evaluated only right before a new connect:
  - free heap >= 51200 B (both record buffers 2 x 16736 B, about 13 KB handshake peak, about 4 KB Wi-Fi/lwIP margin; in v10 a second handshake with the WS up, heap_min about 44 KB, failed with -9984);
  - largest block >= 35520 B (both record buffers + 2 KB): go;
  - largest block 18432..35519 B: probe by allocating the two 16717 B record buffers with the mbedTLS caps and freeing them, go if both fit;
  - largest block < 18432 B: wait. Backoff 5 s doubling to 60 s as before.
  - The record size is derived from the precompiled sdkconfig (16384 content, no dynamic or asymmetric buffers): 13 + 16384 + 16 + 48 + 256 = 16717 B, plus 16 B heap header and light-poisoning canaries.
- **WS guard only before a connect:** the guard ran on every WS pass until `WStype_CONNECTED`, so during the HTTP upgrade the TLS session's own buffers could fail it and stall the upgrade. It now runs only when `ws.loop()` would open a new TCP + TLS connection.
- **Less static DRAM (5104 B on the DSP images):** the OTA download's 2 KB receive buffer is allocated after the TLS session is up (512 B stack fallback). The offline queue's restore and checkpoint buffers (3 KB each) are shared.
- **mDNS waits for the first WS connect:** the first mDNS start of a boot waits until the WS has connected once, or at most 60 s after the IP. Never when the backend has no API key, never in hotspot-only mode (no mDNS there anyway).
- **Diagnostics:** `WS TLS_UP` / `WS TLS_FAILED` with heap free and largest before and after each connect attempt and the mbedTLS error on failure (every attempt for the first 8, then failures once a minute). `MEM TLS_PROBE_OK` when the probe let a connect through. `HEAP_LOW` now names the reason (`low_free`, `low_block`, `probe_failed`) and both thresholds. `OTA DOWNLOAD_TLS_START` and `DOWNLOAD_TLS_UP` log the heap around the download handshake. `device_hello` carries `heap_free`, `heap_largest`, `heap_min` and `heap_largest_min` (telemetry already had them).
- **Over-temperature latch (diagnostic only, DSP):** the reading that tripped the 80 C latch is saved as `ot_c10` (deci-C) next to `ot_latch` and logged in `OVER_TEMP_RESTORED` ("tripped at 84.3 C" or "unknown" for a latch set by an older build). When the latch sets and clears is unchanged.
- Host tests for the guard arithmetic, the verdicts (including the ACE-000001 numbers), the probe path, the backoff and the mDNS wait.

## 2.5.19 (OTA test of the 2.5.18 updater)

Version is 2.5.19 on all four product images so a unit already on 2.5.18 will accept the file. No other functional change on top of 6446e4d.

## 2.5.18 (OTA download robustness, Caleb 12 Nov 2026 16:44 WAT)

On top of 2.5.17 (da2a0a9). All four product images. Prompted by the ACE-000001 OTA of 2.5.17 failing with `download_or_verification_failed` before any download request reached api-dev.

- **Longer TLS handshake for the OTA check and download:** 45 s (was 10 s). TCP connect 15 s (was 10 s). Socket read and body stall 30 s (was 15 s). Whole-body cap stays 180 s. The worker stays on core 1 at priority 1 and the handshake loop yields between mbedTLS steps, so watchdog exposure is unchanged. The install path still makes the output safe first (`prepare`) exactly as before.
- **TLS error logged on download:** a negative GET logs `OTA DOWNLOAD_TLS http=<n> tls=<mbedTLS err> <text> ms=<elapsed>`, like the check path.
- **Specific error codes** replace `download_or_verification_failed`: `download_no_slot`, `download_url_invalid`, `download_connect_failed`, `download_timeout`, `download_http_<code>`, `download_size_mismatch`, `download_too_large`, `download_flash_begin_failed`, `download_stalled`, `download_read_error`, `verify_magic`, `flash_write_failed`, `verify_size`, `verify_identity`, `verify_sha`, `finalize_failed`. New optional `last_error_detail` in `ota_check` and the HTTPS check body. Schema otherwise unchanged. See README "OTA error codes".
- **Hello:** `ota_host` and `ota_url_override` in `device_hello`.
- Host tests for the error mapping and the host/override helpers (`lib/pc_core/src/ota_errors.h`).

## 2.5.17 (OTA test of the 12 Nov "one build with everything" image)

Version is 2.5.17 on all four product images so a unit already on 2.5.13 (or an earlier test image 2.5.14 / 2.5.15 / 2.5.16) will accept the file. No other functional change on top of 3a56262.

## Unreleased (one build with everything Caleb asked for, 12 Nov 2026)

Shipped as 2.5.17 for the OTA test (this tree was held at 2.5.13 until Caleb asked for the bump).

**Battery warnings, Caleb's 8 Nov policy, all four images**
- The local `/events` battery notification and the cloud `battery_soc` event used 20/10/5. Now: `battery_low` at <= 25 %, `battery_critical` at <= 15 % (the one-time shutdown warning, one per episode), output off at <= 10 % (the existing `battery_critical` latch event, code `soc_10pct`), restart blocked until 15 %.
- `shutdown_imminent` (5 %) is no longer sent. The type and enum value are kept so older apps still parse it.
- `battery_critical` stays while the output latch holds and clears only after SoC stays above critical + 3 % for 60 s. `battery_low` clears above low + 3 %.
- The low and critical percents follow the saved Manual values (`/v1/battery-settings` `low_percent`, `warning_percent`), else 25/15. An invalid saved pair falls back to 25/15. Saved values now load at boot on every image (STM loaded them only on the first Manual request, and never fed the notifications).
- Critical message text: "Output turns off at 10% and stays off until the battery is back to 15%. Recharge now." or, while latched, "Output is off to protect the battery. It can turn on again once the battery is back to 15%."
- STM: while the ESP battery latch holds, a manual ON needs BMS SoC >= 15 % (was > 10 %). The latch itself was already 10 % / 15 %. The 48.0 V no-BMS fallback and the STM32 reserve-latch rule are unchanged.

**Manual output power limit events (DSP only)**
- `ac_output_limit`: type `overload`, severity `warning`, code `output_limit_w`, value = PZEM watts. Raised when the load stays above the Manual ceiling or the usable rating for 3 s. One per episode (it re-arms after 10 s under; the clear waits 10 s).
- `ac_output_shutdown`: type `overload`, severity `critical`, code `output_limit_off` (warn_and_shutdown ceiling) or `rated_overload_off` (usable rating), `release_requires` `load_below_limit_30s`. Raised when GPIO15 is turned off, cleared when the latch releases. The output is not turned back on.
- Both go to cloud telemetry `events[]` (offline queue `ev_overload`) and the local `/events` SSE. Titles: "Output power limit exceeded", "Output power back under the limit", "Output off: power limit exceeded", "Output power limit cleared". STM images do not raise them (no PZEM-based enforcement).

**Local hotspot OTA and /unregister (ported from collector 2.5.12, esp32_fw 681ebf2)**
- `POST /ota`: local auth (Basic local token or Bearer session/lan token; the setup credential is refused), required `X-Firmware-SHA256`, streamed via the Update lib. The image must carry this image's own model identity, so another model's image is refused. Version must differ from the running one (the cloud rule); an older one needs `?force=1`. The boot partition is set only after SHA-256 and identity pass. Refused while battery-latched, during a cloud OTA or while the running image is unconfirmed. The WebSocket stays paused and LoRa idles during the upload; an upload idle 30 s is aborted.
- Rollback: in `hotspot_only` a new image is confirmed after 3 min of healthy local run, else the 5 min rollback. Other modes keep the 30 s cloud rule. The cloud OTA path is unchanged.
- `/unregister` pushes SSE `state` at once, logs `SOCK_REMOVE_OK` and clears the saved socket name.
- `tools/ota_upload.py firmware.bin --model <model> [--host] [--force] [--bearer]`. `--model` is required.

**STM estimated output energy save interval**
- Was a flat 15 min (up to 1250 Wh lost at 5 kW on a power cut). Now save when >= 10 Wh is unsaved and >= 5 min have passed, or after 15 min with > 0.5 Wh, or on a day change. Loss is at most about 5 min at rated power (417 Wh at 5 kW, 292 Wh at 3.5 kW). Worst case 288 NVS writes/day plus one day change (was 96 + 1).

## Unreleased (mDNS on the home network)

Caleb approved 2026-11-12 14:31 WAT (via the Software VP). App home Wi-Fi setup could not find units because `net.cpp` only called `WiFi.setHostname()` and nothing started mDNS.

- After the station gets an IP on the saved home network, `MDNS.begin(<display id>)` (e.g. `ACE-000001.local`) and `_http._tcp` on port 80 (`LOCAL_HTTP_PORT`, the server for `/v2/setup/info`) with TXT `device_id`, `model`, `fw`, `path=/v2/setup/info`. Instance name is the display id.
- Hostname: the display id as one DNS label (letters, digits, `-`; other runs become one `-`; max 63), MAC `ACE-DDEEFF` if nothing is left. `pc::mdns` in `lib/pc_core/src/mdns_core.*`, host-tested.
- The `STA_GOT_IP` event only bumps a counter. `net::service()` (loop task) restarts mDNS once per new IP or display-id change, stops it when the station drops, in hotspot-only mode or during a Wi-Fi test, and retries a failed begin after 30 s or on the next got-IP. Hotspot and the 30 s router-loss fallback are unchanged.
- No `powercell.local` alias (two units on a LAN would collide). Version stays 2.5.13.

## Unreleased (DSP orange LED macro renamed)

- `PIN_LED_BLUE` is now `PIN_LED_ORANGE` (DSP orange, mid-SOC, GPIO12). Name only: red GPIO13, orange GPIO12, green GPIO14 and the front light GPIO27 are unchanged, and so is the light behaviour. STM images are unchanged.

## Unreleased (DSP orange and red status light pins swapped)

Caleb 2026-11-12: "Change the light pins swap orange and red on the firmware".

- DSP images only: the orange (mid-SOC, schematic Y, firmware `PIN_LED_BLUE`) and red status lights swap GPIOs. Red is now GPIO13. Orange is now GPIO12. Green stays GPIO14. The front light stays GPIO27. SOC colours are unchanged (70 % and up green, 40-69 % orange/blue channel, under 40 % or no/stale BMS data red).
- GPIO12 is still a strapping pin (MTDI). `leds::begin()` still writes it low before it becomes an output and does not enable a pull-up.
- STM images are unchanged (`LED_MAP_OLD_CODE=1`, red GPIO13, yellow GPIO12, green GPIO27, amber GPIO32).

## Unreleased (live source/status follow grid presence on DSP)

Caleb approved 2026-11-12 13:05 WAT. DSP v0.1.1 units showed `live.source` = battery and `live.status` = inverting with grid connected, because `stm32link::status()` on DSP sets `sys = Inverter` from any PZEM sample and grid presence was not an input to `live::source()` / `live::status()`.

- `pc::live::Inputs` gains `gridKnown` / `gridPresent` (tri-state) and `sysAssumed`. DSP fills them from the debounced GPIO39 presence (`dspGridRead().present`, 170 V on / 160 V off, 2 s), never raw volts, and marks `sys` as assumed. STM fills them from `has_grid && grid_v >= 160 V`, the same rule as `grid_available`.
- Grid override: when grid is known present, the BMS is online and the pack is not discharging (current above -0.5 A), and `sys` is the DSP placeholder or Unknown, `source` = `grid` and `status` = `charging` (BMS current above +0.5 A or charger CHR state) or `idle` (grid pass-through). Existing strings only. A fault still gives `fault`.
- Unchanged: grid present but pack discharging (battery + inverting, or solar when PV covers it), grid unknown or absent, BMS offline, stale PZEM/STM32 data, and every real STM32 SYS report (Inverter, Grid, Idle, Reserve, Fault). `output_power_w`, `load_pct`, payment lock, protections and battery policy are unchanged.
- Units on 2.5.13 builds before grid_sense still publish `grid_available` null and keep the old labels.

## Unreleased (PV energy saved more often)

- Solar NVS (`solar` / tot_wh, tdy_wh, day; same keys so existing totals still load) now saves when unsaved energy is at least 10 Wh and 5 minutes have passed since the last save, or when any unsaved energy above 0.5 Wh has waited 15 minutes, or on a day change. Was 50 Wh and 30 minutes, both required. A power cut now loses at most about 5 minutes of production (~250 Wh at 3 kW). Worst case is 288 writes/day of those 3 small keys, within ESP32 NVS wear levelling. STM estimated-energy (15 min) and PZEM energy-today (baseline/day changes only) are unchanged.

## Unreleased (DSP over-temperature cutoff, sensor fault alert, NTC on GPIO33)

Caleb 2026-11-11 18:30 WAT: "RX is 17, TX is 16, let the fan stop at 60, let there be over-temperature cutoff and alert at 80, the sensor was now moved to pin 33". Then: a faulty sensor must raise a notification alert.

- NTC moved from GPIO36 to GPIO33 (ADC1_CH5). GPIO36 is no longer used. On DSP nothing else touches GPIO33: the STM32 UART (PIN_STM32_RX 33) is never begun with FEATURE_STM32=0 and Dubem's current-sense input is not read. Compile-time checks cover every DSP pin.
- Fan: ON at 70 C or more, OFF at 60 C or less (was 65). A sensor fault still forces it ON. The cutoff does not touch the fan.
- Over-temperature cutoff: a valid reading of 80 C or more latches the cutoff and drives GPIO15 to the off level (`gateway::set(false, "over_temperature")`, retried every 5 s and whenever the output reads on), from the same power-guard pass as the low-SOC cutoff. The latch is saved in NVS (`battery_guard/ot_latch`, beside the low-SOC latch) and re-applied at boot. ON (cloud, local /state, any origin) is refused with `over_temperature_latched` while the reading is 80 C or more, or while latched until a valid reading is below 70 C. Then a user ON clears the latch. Cooling never turns the output on by itself. Payment lock, battery critical and overload keep their earlier refusal codes.
- Alerts, through the event engine like battery_critical: `over_temp` / id `over_temperature`, critical, value = temperature, code `system_ntc_80c_cutoff`, release_requires `cool_below_70c_then_on`. Raised on the latch, cleared when the latch is cleared. `temp_sensor_fault` / id `temp_sensor_fault`, warning, code `ntc_open_short_range`, raised once after 4 s of open, short, out-of-range or stale (no sample for 10 s), cleared after 10 s valid. They go to cloud telemetry events and active_events, the offline queue, and the local notification stream.
- Sensor fault does not cut off the output. The fan is forced ON and the alert is raised. The NTC on its own cannot prove the unit is hot, and a broken wire should not take a customer's power away. A unit already latched stays latched (ON refused) until a valid reading below 70 C.
- Status: live block (cloud telemetry.live, /v2/status live, /data, /metrics) `over_temp_cutoff`, `temp_alert` (`over_temperature` | `temp_sensor_fault` | null), `temp_sensor_fault` (now includes stale). `thermal` object (telemetry, SELFTEST) `over_temp_cutoff`, `on_blocked`, `alert`, `cutoff_c`, `on_allowed_below_c`, `ntc_pin`. `power_guard` `over_temp_cutoff`, `over_temp_blocks_on`. INFO adds `over_temp_cutoff=`.
- BMS: ESP RX GPIO17 / TX GPIO16 on DSP (confirmed). Stale comments fixed.

## Unreleased (DSP system temperature and fan, from Dubem's dspic collector_2)

- DSP images read a 10k NTC (B 3950, R25 10 k) through a 2k7 divider from 3V3, NTC on the 3V3 side (`NTC_ON_LOW_SIDE` 0, Dubem default), first on GPIO36, now GPIO33 (entry above). 16 samples averaged with `analogReadMilliVolts` every 2 s, 11 dB attenuation. Same math and limits as Dubem's thermal.cpp: within 40 mV of 0 V or 3.3 V, or outside 0-150 C, is a sensor fault. A reading older than 10 s is stale.
- Cooling fan on GPIO25, HIGH = ON: ON at 70 C or more, OFF at 65 C (now 60 C, entry above), held in between. A sensor fault forces the fan ON (fail-safe).
- Published as `system_temperature_c` (deg C, null when absent, faulted or stale), `temp_sensor_fault` and `fan_on` in the live block (cloud telemetry.live, /v2/status live, /data + SSE metrics, /metrics), plus a `thermal` object in cloud telemetry and SELFTEST (`temperature_c`, `valid`, `fault`, `fan_on`, `ntc_mv`, `age_ms`). As in Dubem's metrics.cpp, the legacy `inverter_temperature` / `inverter.temperature` keys carry the same NTC value on DSP (the board has no STM32 temperature), so the installed app's system temperature shows it. INFO prints `thermal=ntc10k ntc_pin=33 fan_pin=25 temperature_c=... fault=... fan_on=... ntc_mv=... over_temp_cutoff=...`.
- GPIO25 was the STM power light pin; it is not used on DSP. STM images unchanged.

## Unreleased (DSP grid crosstalk and offline energy today)

- DSP grid sense (GPIO39): inverter output can couple into the grid divider and read 36-88 V with no mains. Presence is still 170 V on / 160 V off, now with a 2 s debounce so a brief spike does not flip the flag. Below that window, published `grid_voltage` / `gridVoltage` and any grid frequency are JSON null and `grid_available` is false. The raw ADC reading is only `grid_sense_raw_v` (telemetry, /data metrics, /metrics, /v2/status live, SELFTEST). Crosstalk while the inverter is on cannot mark grid. 230 V still does. STM images unchanged.
- Energy today on DSP: with no trusted clock (offline, "Needs internet", no NTP/phone, RTC untrusted) the first valid PZEM lifetime reading now starts a session baseline, so `energy_today_kwh` is published instead of null. A reboot keeps that baseline. When a trusted clock arrives, the session is adopted as the local day and the energy already counted stays. Midnight still rolls only on a synced clock.

## Unreleased (DSP PZEM bench diagnosis)

- Bench 2026-11-10 22:34 WAT on 701fa96: GPIO15 ON works; PZEM never replied (201 polls, 0 bytes). Dubem's firmware reads the same meter on the same board.
- Compared byte by byte with Dubem's pzem.cpp: same UART1 (Serial1), same begin(9600, SERIAL_8N1, rx=35, tx=32), no invert, same request F8 04 00 00 00 0A + CRC, same flush, 200 ms timeout and 1 s poll, BMS on Serial2 in both (no UART sharing). No code path in this firmware calls pinMode, ADC, LEDC, gpio_reset_pin or a second begin on GPIO32/35.
- New: every poll reads back the UART1 routing (U1RXD input select, GPIO32 output select, IO_MUX function and input enable, RTC mux of both pads). A change is logged once as PZEM ROUTE_LOST with the raw registers and repaired (uart_set_pin, rtc_gpio_deinit, begin if the driver is gone), then ROUTE_RESTORED or ROUTE_STILL_BAD. Boot logs PZEM ROUTE. SELFTEST pzem has route and route_heals.
- New console command PZEM RAW (DSP only, read-only): prints the routing registers and the RX pin level, sends one read request and hex-dumps every byte received for 500 ms. It never sends the energy reset.

## Unreleased (DSP battery status light)

- DSP builds: the thunderbolt light now matches Dubem's lighting.cpp. Red 12, blue 13, green 14 on LEDC 5 kHz 8-bit, one lit at a time. SOC 70 % and up green, 40-69 % blue, under 40 % red. No BMS frame, or one older than 15 s, is red. With grid present (GPIO39, our 170/160 V hysteresis) the lit LED breathes on a 4 s triangle; without grid it is solid. Caleb confirmed 2026-11-10 21:13 WAT.
- This replaces, on DSP only, the original profile's indications: the critical-latch 100 ms red blink, STM32 fault red, and blue for offline, hotspot, or SOC under 25 %. Those states are no longer shown on the light on DSP (they stay in SELFTEST and the logs). STM builds are unchanged.

## Unreleased (DSP front light)
- DSP images: the front light on GPIO27 (HIGH = ON) follows the inverter like Dubem's firmware: on with ON, off with the boot latch and every off path. Caleb approved 2026-11-10 21:10 WAT. STM images unchanged.

## Unreleased (DSP fixes from Dubem's working firmware)
- DSP GPIO15 now actually switches. The pad resets into its MTDO (JTAG) IO_MUX function and the DSP path never selected the GPIO function, so ON and OFF writes did not reach the pin. Each write now selects the GPIO function first (`esp_rom_gpio_pad_select_gpio`), releases the hold, sets the level, and holds only at the off level. HIGH = ON unchanged.
- DSP BMS: both ratings listen for JK-PB at 115200 on RX GPIO17 / TX GPIO16, matching Dubem's working firmware (`JK_RX 17`, `JK_TX 16`, `JK_BAUD 115200`). The 3.5 kW image no longer polls JBD at 9600 on RX16/TX17, which returned no data.
- PZEM: SELFTEST `pzem` block (rx, tx, baud, polls, ok, timeouts, short_frames, bad_frames, fail_run, last_ok_age_ms, live values). `PZEM NO_DATA` warning on the first failed poll and at most every 30 s while it keeps failing, `PZEM RECOVERED` once, `PZEM INIT` at boot. The request is flushed before the reply timeout starts, as Dubem's driver does.

## Unreleased (DSP 5 kW JK-PB BMS)
- DSP 5 kW images (`esp32dev_dsp011_5kw`) listen for JK-PB 300-byte frames at 115200 on the existing DSP BMS pins (RX GPIO16 / TX GPIO17, `DSP_BMS_UART_SWAP` 0). Caleb confirmed 2026-11-10 17:47 WAT that the unit uses the same JK-PB model as the STM images, over TTL UART. Build flag `DSP_BMS_JKPB=1`. Stale aging, no-zero publish, BMS-own SOC latch policy and the BMS RAW pause/idle hook are the same as the STM JK path. INFO printed `bms=JK-PB baud=115200 rx=16 tx=17`. Superseded: the DSP BMS is ESP RX GPIO17 / TX GPIO16 (see "DSP fixes from Dubem's working firmware"; Caleb confirmed 2026-11-11 18:30 WAT), INFO now prints `rx=17 tx=16`.
- DSP 3.5 kW images stay on the JBD 9600 polled driver (`DSP_BMS_JKPB` defaults to 0). Superseded: both DSP ratings now use JK-PB. STM images unchanged.

## Unreleased (DSP v0.1.1 PZEM pins)
- DSP images only: PZEM UART1 moves to RX GPIO35 / TX GPIO32 (was RX GPIO32 / TX GPIO33), 9600 8N1, per Caleb 2026-11-10 13:16 WAT. GPIO33 is unused on DSP. STM images unchanged.
- GPIO35 is input only with no internal pull-up. The PZEM open-collector TX needs a board pull-up to 3.3 V on GPIO35, and the PZEM TTL side must be 3.3 V safe (no 5 V pull-up).
- No ADC read remains on GPIO35 (DSP grid sense is GPIO39 only; FEATURE_BAT_SENSE_ADC=0 on DSP).

## 2.5.13 (NVS model)
- NVS Powercell model adds `PC3_5K` (3.5 kVA / 3.15 kW usable, matching the 35kw
  factory rating). Aliases `3.5K`, `PC3.5K`, `35K`, `PC35K`. `PC3K` / `PC5K` /
  `PC10K` unchanged for 2.5.11 units. `tools/factory.py --model` accepts the new
  code and warns when `--model` disagrees with INFO `hardware_model`.


## 2.5.8 (2026-11-04)
- App solar tile offline (installed Powercore 1.0.39/1.0.40 reads GET /data root `solar.{online,voltage,current,power}` via FirmwareSnapshot._solar; /data had no root `solar`, so the tile showed `--`). GET /data + SSE now carry root `solar`: `online`, `valid`, `i2c_failing`, `voltage` (V), `current` (A), `power` (kW, so the app's displayKw >20 = W rule is right for any PV power), `power_w`, `energy_today_kwh`, `energy_total_kwh`. Null values / `online:false` when no valid ADS1115 read in 5 s.
- PV energy: integrated from V x A on every valid read (gaps > 10 s and negative power not counted). `today` resets at local midnight (trusted clock: NTP or phone POST /v2/time, plus the phone tz); without a trusted clock it keeps counting on the last known day. `total` is lifetime. NVS namespace `solar` (tot_wh, tdy_wh, day), written when total grew >= 50 Wh and >= 30 min since the last write, or at a day change.
- Energy also in cloud `telemetry.solar.energy_today_kwh/energy_total_kwh`, live block `solar_energy_today_kwh/solar_energy_total_kwh` (telemetry.live, /v2/status live, /data metrics), SELFTEST solar block and the `SOLAR` line (`today_kwh= total_kwh=`).
- NOTE: the installed app's "Today x kWh" caption reads `device_data.days.solar_input[<weekday>]`, which its offline /data mapper (LocalReading.toCloudPayload) does not pass through, so the caption needs an app change; the tile value, Voltage and Current are filled by this firmware.
- Host tests: solar energy integration, day rollover, save policy, app kW rule.

## 2.5.7 (2026-11-04)
- PV in the app live block: `solar_valid`, `solar_voltage`, `solar_current`, `solar_power_w` (null when no valid ADS1115 read in the last 5 s) in cloud WS `telemetry.live`, `/v2/status` `live`, and flat in /data + SSE `metrics` (beside the existing `solarVoltage`/`solarCurrent`/`solarPower`/`solar_valid`). Cloud `telemetry.solar.*` unchanged.
- Console `SOLAR`: one line with valid, V, A, W, i2c_errors, failing and last good read age (`age_ms`, `never` if none since boot).
- SELFTEST solar block adds `valid`, `voltage`, `current`, `power_w`, `last_good_age_ms`.
- Host tests: pc::live::solarView / formatSolarLine.

## 2.5.6 (2026-11-04)
- STM32 XFER4 support: when a manual/cloud ON passes the on-gate (BMS SoC > 10 %, or no BMS and pack >= 48.0 V) and the STM32 reports RSV:1, the ESP sends `RSV CLR` first (500 ms reply wait), logs the reply (PWR RSV_CLR), then sends ON. `RSV:ERR,VBAT|FAULT|BUSY` refuses the ON with that reason; no reply (pre-XFER4 STM32) sends ON anyway. The clear is never sent at or below the gate.
- Legacy parser: `RSV:OK` / `RSV:ERR,...` routed as command replies (not the RSV:0/1 record field); `TEMP: x.xC  NTC_RAW: n` parsed (inverter_temperature from TEMP; ntc_raw in SELFTEST stm32 block).
- Host tests: parseRsvReply + XFER4 parser cases.

## 2.5.5 (2026-11-04)
- Power-on gate uses battery percentage (Caleb): a user/cloud ON is refused only when the BMS SoC is <= 10 %. Above 10 % the ON clears the ESP critical latch and is sent even while the STM32 reports RSV:1. BMS offline/invalid: fallback pack voltage >= 48.0 V (16S LFP ~10 %); no SoC and no voltage: old latch rule. Auto restore keeps the 15 %/60 s hysteresis. Pure logic `pc::soc::onGate` + host tests.
- NOTE: the 52 V release lives in the STM32 firmware (config.h VBAT_RESERVE_RELEASE_V, system_control.c); the STM32 has no UART command to clear it, so the STM32 still holds the inverter off until its own rule is changed.
- CORE_DEBUG_LEVEL=0: removes the raw Arduino core `[E][Wire.cpp:513] i2cRead returned Error 263` lines (and other core log_e lines). Firmware LOG* lines are unaffected.

## 2.5.4 (2026-11-04)
- Solar/I2C: ADS1115 read directly with bounded single-shot transfers (20 ms Wire timeout,
  OS-bit poll capped) instead of the Adafruit busy-wait. After 3 failed cycles the reader
  backs off 1 s doubling to 30 s; PV values go null (`solar.valid=false`) at once; one WARN
  `SOLAR I2C_FAIL` and one INFO `SOLAR RECOVERED`; Wire/i2c IDF log tags silenced so
  `i2cRead returned Error 263` no longer floods the console. Runs in its own task, so an I2C
  stall cannot block loop().
- New app live keys (README "App live fields") in /data + SSE `metrics`, WS `telemetry.live`
  and /v2/status `live`: source, status, output_voltage/current, output_power_w (+source),
  load_pct, inverter_temperature, model, rated_kva, usable_kw, rated_kw, battery_current_a,
  battery_power_w, remaining_ah, full_capacity_ah, cycle_count, soh, time_to_empty_min
  (smoothed), time_to_full_min.
- Powercell model: `SET model PC3K|PC5K|PC10K|none` (NVS pc_cfg/model, unset by default),
  factory.py `--model`. Usable kW = 90 % of kVA (assumption). Shown in INFO, SELFTEST and
  /v2/setup/info (`powercell_model`, `rated_kva`, `usable_kw`). (`PC3_5K` added in 2.5.13.)
- SELFTEST: stm32 temp_c/vout now null when not sent, adds iout, sys and a solar block.
- /data snapshot: if it would exceed the 4 KB scratch, metrics.sockets and old notifications
  are dropped (one WARN) instead of sending truncated JSON.
- NVS: new key pc_cfg/model (cleared by FACTORY_WIPE and FACTORY_WIPE ALL, kept by the customer
  factory reset). No other layout change.

## 2.5.3 (2026-11-03)
- Battery-critical latch deadlock fix. The latch (SoC <= 10 %, saved in NVS) now also releases
  when the BMS is online with a valid frame and BMS SoC >= 15 % held 60 s, with no charging
  needed. A latch restored from NVS (set while the BMS pins were wrong) no longer stays on
  forever on a full idle pack. STM32/ADC voltage fallbacks still cannot release it without
  charging (BMS > +1 A or STM32 BULK/ABSORB/FLOAT held 60 s). Latch level 10 %, release 15 %.
- CRITICAL_RELEASE log line now shows the SoC source.
- No NVS layout change.

## 2.5.2 (2026-11-03)
- JK BMS UART2 pins fixed to rx=17 tx=16, matching the Dubem firmware proven on this hardware
  (`Serial2.begin(115200, SERIAL_8N1, 17, 16)`). 2.5.1 listened on IO16 and received nothing,
  so pack voltage read 0 and the battery_critical latch was never released.
- New build flag `JK_BMS_VERIFY_CHECKSUM` (default 1). Set 0 to accept frames with a bad
  checksum, as the Dubem parser did.
- TEMPORARY console command `BMS RAW`: pauses the BMS parser and hex-dumps the next 64 bytes
  from Serial2 (2 s timeout).
- No NVS layout change.

## 2.5.1 (2026-11-03)
- Legacy local routes (GET /data, POST /state, /relay, /events SSE, every requireLocal/postRoute
  route) now accept `Authorization: Bearer <session|lan_token>` from /v2/setup/passcode or
  /v2/login, mapped to device scope (Cred::Local). Fixes app 1.0.38 getting 401 "Unauthorized"
  on offline controls after onboarding.
- Bearer failures are not counted toward the auth limiter (same as /v2 routes); a locked
  limiter still rejects Bearer. Basic auth with local_token is unchanged and still counts.
- /events SSE: the auth check is header-based, so the app must use a header-capable SSE client
  (browser EventSource cannot send Authorization).
- No NVS layout change: a normal upload keeps passcode, serial, secrets, pairings.
