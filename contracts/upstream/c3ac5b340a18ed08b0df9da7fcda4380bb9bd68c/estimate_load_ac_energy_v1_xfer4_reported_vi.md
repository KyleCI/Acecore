# xfer4_reported_vi_pf080_unvalidated_v1

This is a separate estimate profile inside the `estimate.load_ac_energy.v1` envelope. It is not a relabeling of the RMS model. RMS records keep `source_path` `ac_vrms_mv_x_ac_irms_ma_x_pf_millis`, inputs `ac_vrms_mv` / `ac_irms_ma` / `ac_v_valid` / `ac_i_valid`, and per-interval `floor`. They are not rewritten when this profile is added.

The collector is not implemented. Nothing here authorizes a live device to start sending this model.

## Recognized profile

| Field | Value |
| --- | --- |
| `model_id` | `xfer4_reported_vi_pf080_unvalidated_v1` |
| `source_path` | `reported_voltage_mv_x_reported_current_ma_x_pf_millis` |
| `source_identity` | `Powercell_caleb_xfer4_flash.zip` |
| `source_sha256` | `df46e024a3cfdeafd347ed6dc33e2402d470a546ea085b55b215c50c56cc4462` |
| `provenance_basis` | `user_identified_last_flash` |
| `validation` | `unvalidated` |
| `pf_kind` | `assumed` |
| `pf_assumption_millis` | `800` |
| `coverage_scope` | `inverter_output_only` |

Provenance is the user confirming that zip as the last flash, with candidate code and binary strings corroborating the profile. It is not a calibration and not a readback of the running image.

XFER4 reports inverter `VOUT` in volts and `IOUT` in amps. The contract inputs are `reported_voltage_mv` and `reported_current_ma` as decimal strings. There is no transmitted RMS valid flag. `ac_v_valid`, `ac_i_valid`, `ac_vrms_mv`, and `ac_irms_ma` on this profile are `rms_fields` and are rejected.

`IOUT` `0.00` means the report is below `0.18 A` (`180` mA) or is actually zero. The sample is `clamped_or_zero`. It is not a valid zero. A reported current from `1` through `179` mA is `below_clamp` because this profile does not transmit it as a non-zero value.

## Integration

For one covered span, with the series remainder carried in:

```
numerator = reported_voltage_mv * reported_current_ma * 800 * covered_ms + remainder_in
mWh       = numerator / 3600000000000
remainder_after = numerator mod 3600000000000
```

The integer `mWh` is the only energy added to the counter. `remainder_after` stays on that canonical record and is the next interval's `remainder_in`. The backend checks the declared counter. It does not multiply the stored interval by `0.800` again. A gap or a clamped report adds a zero product, so its remainder passes through unchanged.

`230000` mV, `1000` mA, `3600000` ms, remainder `0` is `184000` mWh. A `19` ms span at those inputs emits `0` mWh and keeps a non-zero remainder. The next `19` ms emits that fraction. Sending `0` for the second span is `remainder` and is not acknowledged.

The previous sequence must already be stored. A missing predecessor is `sequence_gap`. The missing span is not invented.

## Samples

| `sample_validity` | Meaning |
| --- | --- |
| `reported` | Non-zero reported voltage and reported current of at least `180` mA. Unverified. |
| `clamped_or_zero` | Reported current `0`. Energy contribution `0`. UI: `Below reporting threshold or zero; consumption may be missed.` |
| `gap` | `gap_reason` is `grid`, `transfer`, `fault`, `stale`, `truncated`, or `txdrop`. No inputs. Covered time `0`. Not counted as load. |

`common_output` time is not energy for this profile. Uncertain time and a Lagos midnight crossing stay unallocated. They are not split into the two days.

## Series, ACK, and opt-in

Identity, journal spelling, and the ACK keys are the existing `estimate.load_ac_energy.v1` mechanics. The ACK echoes the incoming `device_id`, `meter_epoch`, `boot_id`, `seq`, and `clock_correction_version`. It does not add a model field. The stored row's `device_id` is still the stored owner. `model_id`, `source_sha256`, and `validation_state` are new nullable columns. RMS rows leave them null.

A different `model_id`, source hash, or PF in an existing `meter_epoch` + `boot_id` is `model_epoch`. The old body stays. The new series uses a new `meter_epoch`. The same body retries the same `record_id`. A different body for that identity is `conflicting_duplicate`.

An unknown `model_id` or source path is `unknown_model`. It is not parsed as RMS.

The profile is accepted only when `ESTIMATE_REPORTED_VI_OPTIN` is exactly `xfer4_reported_vi_pf080_unvalidated_v1`. Otherwise the result is `model_not_selected`. That variable is not set for deployment. Selection in a synthetic test is not authorization to enable it on a live device.

Daily totals do not add this profile and the RMS profile into one unlabeled number. A day with both is `separate_models` and `allocated_mwh` is null. `reported_vi.blended_with_measured` is false. There is no PZEM term and no error bound.

UI copy when this profile is present:

- `Estimated inverter energy · PF assumed 0.8 · sensor values unverified`
- `Inverter output only. Grid, transfer, fault, stale, truncated, and dropped reports are gaps and are not counted as load.`
- `Report coverage is not accuracy and is not whole-output consumption.`
- Clamp, when a clamped sample is in the read: `Below reporting threshold or zero; consumption may be missed.`

`duration_coverage_is_accuracy` stays false. A ratio of `1.000` does not mean the watt-hours are accurate or that they cover the whole output.

## What is still closed

- No collector and no firmware change in this repository. A device does not emit this record yet.
- `ESTIMATE_REPORTED_VI_OPTIN` is off unless a process sets it. This PR does not set it in deployment config.
- The zip hash is not proof the chip is running that image. There is no readback.
- Reported volts and amps are not validated RMS. There is no accuracy band.
- Current under `0.18 A` is indistinguishable from zero on this report, so that consumption is missed.
- Grid, transfer, fault, stale, truncated, and dropped intervals are holes. They are not filled.
- The fractional remainder cannot be reconstructed if the previous sequence was never stored.
- Live STM32 eligibility stays off.
