# Collector firmware changelog

## Unreleased (DSP 5 kW JK-PB BMS)
- DSP 5 kW images (`esp32dev_dsp011_5kw`) listen for JK-PB 300-byte frames at 115200 on the existing DSP BMS pins (RX GPIO16 / TX GPIO17, `DSP_BMS_UART_SWAP` 0). Caleb confirmed 2026-11-10 17:47 WAT that the unit uses the same JK-PB model as the STM images, over TTL UART. Build flag `DSP_BMS_JKPB=1`. Stale aging, no-zero publish, BMS-own SOC latch policy and the BMS RAW pause/idle hook are the same as the STM JK path. INFO prints `bms=JK-PB baud=115200 rx=16 tx=17`.
- DSP 3.5 kW images stay on the JBD 9600 polled driver (`DSP_BMS_JKPB` defaults to 0). STM images unchanged.

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
