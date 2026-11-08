# estimate.load_ac_energy.v1

Interim AC load energy for original Powercell v0.1. This is an estimate from valid AC RMS voltage, valid AC RMS current, and a declared power-factor assumption. It is not a calibrated measurement, not battery DC discharge, and not PV yield.

Firmware that shares this contract should send the same JSON. The canonical schema is `powercell-app/backend/contracts/estimate.load_ac_energy.v1.schema.json`. The synthetic fixture is `powercell-app/backend/contracts/demo_fixture.php`. Nothing in that fixture is a hardware observation.

## Formula

```
P_est (W) = (Vrms_mV / 1000) * (Irms_mA / 1000) * (PF_millis / 1000)
mWh       = floor(Vrms_mV * Irms_mA * PF_millis * covered_ms / 3600000000000)
```

`mWh` counters are decimal strings, not JSON numbers. uint64 does not fit a PHP integer or a JavaScript number. The model id is:

- revision `estimate.load_ac_energy.v1`
- source_path `ac_vrms_mv_x_ac_irms_ma_x_pf_millis`
- version `0.1.0`
- `pf_kind`: `assumed`
- `pf_assumption_millis`: `800` (power factor 0.800). This is the original v0.1 assumption. It is not a measured power factor.
- `coverage_scope`: `inverter_output_only`, `common_output`, or `unknown`

Firmware integrates PF 0.800 into the interval counter before sending it. The backend checks that the tagged counter matches the inputs once, then stores that counter. It does not multiply the stored interval by 0.800 again. A record that claims a different power factor, or `pf_kind` other than `assumed`, is rejected and does not rewrite a stored row. A clock correction cannot change the counter or the model. Days list every model that contributed; they are not blended into one unlabeled total. This assumption does not apply to a future measured watt-hour stream.

Integer division truncates. A value one milliwatt-hour higher or lower is rejected. Energy is not multiplied by 1.18. BMS DC discharge is not copied into this stream.

## Namespaces

| Stream | v0.1 |
| --- | --- |
| `estimate.load_ac_energy` | this contract |
| `estimate.pv_energy` | unavailable |
| `measured.load_ac_energy` | unavailable future stream. No PZEM is assumed. |
| `dc.bms_discharge` | not AC load watt-hours. Not accepted here. |

## Identity and retries

A record is unique on `device_id + stream + meter_epoch + boot_id + seq + clock_correction_version`.

- The same body again is a replay. The original row is acknowledged again and is not inserted twice.
- The same identity with a different body is a conflicting duplicate. It is rejected, logged, and not stored.
- The database transaction commits before the success acknowledgement is returned.
- A new `boot_id` or `meter_epoch` restarts the counter. The new UTC interval must not overlap a live row, so the reset is not added on top of the same time.
- Consecutive sequence numbers must continue `counter_end` into the next `counter_start` and must not move monotonic time backwards.
- A gap in sequence may arrive late. The missing span is not backfilled.
- `clock_correction_version` may replace the UTC interval of the same energy counter. The older version is superseded and is not added again. A correction that changes the counter is rejected.

## Time and Lagos days

Instants are UTC. An ISO string must include `Z` or a numeric offset. A local object may use `epoch_ms` (absolute UTC) or `local` plus `offset`. Offsets accept `+01:00`, `+0100`, `100` (HHMM), `1` (hours, because the absolute value is 0–14), and `60` (minutes).

Days are `Africa/Lagos` (UTC+1, no daylight saving). The database session time zone is not used. An interval that crosses Lagos midnight is left unallocated. It is not split by the fraction of the hour. An interval that ends exactly on midnight belongs to the day that just finished.

`time_quality` `uncertain` or `absent`, or a null UTC pair, is unallocated. Unknown source-mode time makes the whole interval unallocated. Invalid or absent samples must carry `0` mWh and are not treated as a valid zero.

## Daily read

`GET /api/energy/estimate/daily?device_id=&date=&from=&to=`

The signed-in user must own the device. `date`, `from`, and `to` are Lagos dates. The default window is seven days ending on the Lagos today.

Each day is one of:

- `missing` — no allocated record. `allocated_mwh` is null.
- `partial` — some estimated energy, with a coverage gap or a partial sample. Uncovered energy is unknown, not zero.
- `valid_zero` — records exist, the counter did not move, and coverage is complete.
- `allocated` — covered estimated energy with complete samples.

`duration_coverage_ratio` is covered time divided by interval time. `duration_coverage_is_accuracy` is false. There is no accuracy percentage.

`GET /api/get-all-data` and `GET /api/weekly_compare/index.php` add `energy_estimate`. Existing keys, including `this_week` and `last_week`, stay as they were.

Ingest is `POST /api/telemetry/energy-estimate` with the same hardware key and device secret as other telemetry, or a device-socket message with `type` `energy_estimate`. There is no public release route for this feature.

## Migration

`021_estimate_load_ac_energy.sql` is the next filename after `020_user_avatar.sql`. This commit, `origin/main`, and `origin/cursor/notification-alerts-ac18` each have one `020` file and no `021`. `schema_migrations` stores the filename, so `021` applies beside either deployed `020`. The live `api-dev` image was not modified.
