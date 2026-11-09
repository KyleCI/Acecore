# Power units

One Powercore app reads every Powercell. The collector normalizes each source
before it fills an existing shared key. Power on a watt key is watts. If a
source reports kilowatts, the collector multiplies by 1000. A reading of 15 W
stays 15. The app does not receive a different key per device, and it is not
asked to guess watts from kilowatts.

Apparent power stays on the keys that already meant apparent: `inverter.output_power`
and `metrics.inverterLoad`. Those values are V times I, in VA, and
`output_power_quantity` is `apparent_va`. They are not copied into
`last_data.power`, `output_power_w`, or `live.output_power_w`.

Active power on those three keys is PZEM active watts (`power_dW` / 10). The
STM32 text path has `VOUT` and `IOUT` in volts and amps and no watt or kilowatt
field, so its active-watt keys stay null. Energy counters that are already
watt-hours stay watt-hours. `energy_get_kwh()` is not published.

`GET /v2/setup/info` adds two fixed top-level fields on every image, next to
`device_id`. The names and values match
`powercell-app/docs/setup_info_units.md` on letambari/official-powercell-app
PR 8:

```json
"units": {"power": "W", "energy": "Wh"},
"data_format_version": 1
```

`data_format_version` is the integer 1 for this firmware. A collector that
omits it, including deployed 2.5.11, is the legacy case and the app applies
its version rule. These fields do not rename or rescale any existing key.

`units.power` is `W` for unit-less power keys only: `last_data.power`,
`inverter.output_power` when it is not flagged `apparent_va`, `battery.power`,
and `solar.power`. `units.energy` is `Wh` for unit-less energy such as
`inverter_energy`.

`inverter.output_power` and `metrics.inverterLoad` from this collector are
V times I and stay flagged `apparent_va`. That quantity is volt-amps, not
active watts. The STM32 text path has no watt field, so its watt keys are
JSON null.

A key whose name already states the unit is outside `units`: `usable_kw`,
`rated_kva`, `*_w`, `lifetime_wh`, and `energy_today_kwh`.

The app treats a negative power, or a power above 1.5 times usable, as
unavailable. This firmware does not publish placeholder sentinels such as
`-1` or `99999` on a power key. Those values are JSON null.

Keys whose names already say kilowatts stay kilowatts. They are not watt keys:

| Key | Unit | Where |
| --- | --- | --- |
| `usable_kw` | kW | setup info, `/data`, live, metrics |
| `rated_kva` | kVA name; this firmware sends null | setup info |

Watt and VA keys:

| Key | Unit | Source |
| --- | --- | --- |
| `last_data.power` | W | PZEM active; null on STM32 |
| `metrics.output_power_w`, `live.output_power_w` | W | PZEM active; null on STM32 |
| `inverter.output_power`, `metrics.inverterLoad` | VA | V times I, flagged `apparent_va` |
| `battery.power`, `battery_power_w` | W | JK pack power |
| `solar.power`, `solar.power_w`, `solarPower` | W | ADS1115 watts, both names |
| `ac_consumed.active_power_w` | W | PZEM active |
| `ac_consumed.lifetime_wh`, `inverter_energy` | Wh | meter or BMS integration |
| `rated_active_w`, `usable_active_w`, `factory_rated_active_w`, `stored_rated_active_w` | W | factory image and NVS |
| `ceiling_w` | W | Manual AC ceiling and overload compare |

Battery low, warning, inhibit, and restart values are percents, not power.
