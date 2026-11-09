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
