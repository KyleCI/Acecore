# Collector firmware changelog

## Unreleased (one build with everything Caleb asked for, 12 Nov 2026)

Version stays 2.5.13.

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
