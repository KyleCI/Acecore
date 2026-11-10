# Power and energy units

`GET /v2/setup/info` declares the units of keys that do not name a unit. The declaration is the same on `esp32dev_stm01_5kw`, `esp32dev_stm01_35kw`, `esp32dev_dsp011_5kw`, and `esp32dev_dsp011_35kw`:

```json
"units": { "power": "W", "energy": "Wh" },
"data_format_version": 1
```

`data_format_version` is the integer 1. No other `/v2/setup/info` field changes.

This matches the installed-app contract in `powercell-app/docs/setup_info_units.md` (app pull request 8). The app uses these fields to convert. When they are absent, it keeps its 2.5.11 legacy rule.

## What the declaration covers

`units.power` is watts for unit-less power keys only:

- `last_data.power`
- `inverter.output_power`, when `output_power_quantity` is not `apparent_va`
- `battery.power`
- `solar.power`

`units.energy` is watt-hours for unit-less energy keys such as `inverter_energy`.

Keys that already name a unit keep that unit: `usable_kw`, any `*_w` key, `lifetime_wh`, `energy_today_kwh`, and `rated_kva`. A value flagged `apparent_va` stays volt-amps and is never copied onto an active-watt key.

## What each image publishes

Stock `esp32dev` has no watt register on the STM32. `output_power` and `inverterLoad` stay volts times amps and are flagged `apparent_va` only on the hardware v0.1.1 images. The watt keys (`output_power_w` and the other unit-less watt fields the STM32 does not measure) are JSON null.

The hardware v0.1.1 images fill `output_power_w` with PZEM-004T active watts and `lifetime_wh` with PZEM lifetime watt-hours. `output_power` remains volts times amps with `output_power_quantity` set to `apparent_va`. A missing, stale, or sentinel reading is null, not zero.

`live.lifetime_wh` is that raw PZEM counter. Cloud telemetry includes it on every frame. It is JSON null only when the meter read is missing, stale, or a sentinel. It is never replaced with zero.

## Today's AC output energy (hardware v0.1.1)

DSP images publish consumed AC output energy since the local-day PZEM baseline, in kWh. The same number is written in cloud telemetry and in local `/data`:

- top-level `energy_today_kwh`
- `inverter.energy_today_kwh`
- `live.load_energy_today_kwh` (also flat in `/data` `metrics`, and on `/data` `live`)

Each of those objects also has `energy_today_source`. When the value is known the source is `pzem004t_v3_active`. When the day total is unknown both the kWh key and `energy_today_source` are JSON null.

`solar.energy_today_kwh` and `live.solar_energy_today_kwh` stay solar-panel energy on every image. On v0.1.1 they are null when the ADS1115 has no measurement. STM32 images keep that PV behaviour and do not copy the estimate into those keys.

The PZEM baseline is the lifetime counter at the first trusted sample of the local day, or the baseline saved earlier that same day. A reboot keeps energy the meter counted while the ESP was off. For the PZEM path, local midnight uses the configured timezone offset. The day rolls only when the clock is synced (NTP, phone time, or a coherent DS3231). An unsynced clock does not invent a midnight and does not publish a value. A missing or invalid meter sample does not publish zero. A counter reset or an implausible jump leaves the rest of that local day unknown. Energy used after a midnight power-off and before the first trusted sample is not included.

## Estimated AC energy (STM32 images)

STM32 images have no watt-hour meter. Each fresh status record (legacy text or framed XFER4) supplies AC output volts and amps. Active power is `V * I * 0.8`. Watt-hours are that power times the monotonic interval. The DS3231 and NTP store UTC. Local midnight is that UTC time plus 60 minutes (Africa/Lagos, WAT, no DST). It is not the RTC's civil clock and it does not follow a phone timezone that is not WAT.

The interval is the monotonic millisecond clock. A slice is dropped when that gap is above 10 seconds (records are about 1 second apart, and a reading is stale at 6 seconds), when monotonic time goes backwards, or when the wall clock jumps by more than the monotonic interval plus 2 seconds. Voltage above 280 V, current above 32 A or below 0, and a loaded output below 80 V are rejected. 32 A is the 5 kW profile at 200 V and PF 0.8; the 3.5 kW profile is lower. An invalid sample adds nothing. It is not stored as 0 W.

The same consumed-energy keys as the PZEM path are published in cloud telemetry and local `/data`, with `energy_today_source` = `stm32_estimated_pf080`:

- top-level `energy_today_kwh`
- `inverter.energy_today_kwh`
- `live.load_energy_today_kwh` (also flat in `/data` `metrics`, and on `/data` `live`)

When the day total is unknown those kWh keys and `energy_today_source` are JSON null together. The day rolls only on a trusted clock (NTP, phone time, or a coherent DS3231). An unsynced clock does not publish a today value and does not invent a midnight.

`live.estimated_lifetime_wh` is the running estimate in watt-hours. It is sent on every telemetry frame and on `/data` `live` / `metrics`. `live.estimated_lifetime_source` is `stm32_estimated_pf080`. `live.lifetime_wh` is absent on STM images so the estimate is not labelled as a measured PZEM counter. The published lifetime is the last value stored in NVS, and it is not allowed to decrease. NVS is written at a day change and at most once per 15 minutes while energy is accumulating. Power loss can drop at most 15 minutes of the image rating: 1250 Wh at 5000 W, 875 Wh at 3500 W.

Fifteen watts stays 15. If a source reports kilowatts, the collector multiplies by 1000 before writing the key. Placeholder sentinels `-1` and `99999` are not published on power keys; those keys are null. A real measurement is left as measured. The app treats a negative power reading, or one above 1.5 times usable power, as unavailable.
