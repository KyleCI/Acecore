# Power and energy units

`GET /v2/setup/info` declares the units of keys that do not name a unit. The declaration is the same on `esp32dev`, `esp32dev_hw_v011_5kw`, and `esp32dev_hw_v011_35kw`:

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

Fifteen watts stays 15. If a source reports kilowatts, the collector multiplies by 1000 before writing the key. Placeholder sentinels `-1` and `99999` are not published on power keys; those keys are null. A real measurement is left as measured. The app treats a negative power reading, or one above 1.5 times usable power, as unavailable.
