# estimate.load_ac_energy.v1

Interim AC load estimate for the original collector (STM32 + LoRa + JK BMS + ADS1115). This is not a measured watt-hour.

## Formula

For a sample that has independently acquired, valid AC `Vrms` and `Irms`, a known mode, and a known coverage scope, over a monotonic `dt` that is not longer than the staleness bound and does not cross an unknown gap:

```text
estimated_mWh = floor((Vrms_mV * Irms_mA * pf_assumed_millis * dt_ms + residue) / 3600000000000)
```

`pf_assumed_millis` is `PF * 1000`. The original v0.1 default is **800**, so `PF = 0.800`. Caleb set that assumption on 2026-11-08. `pf_is_assumption` is always true. The number is not a measured power factor and not an accuracy claim.

Worked example, exact: `230000 mV * 1000 mA * 800 * 3600000 ms / 3600000000000 = 184000 mWh`.

`model_revision` **2** is this assumption. `assumption_provenance` is `caleb-2026-11-08-default-0.8` only while the active factor is 800 on model 2. A later `set_pf_assumed` writes the new factor on subsequent records as `explicit-pf-assumption` and does not edit older records.

## What is not an input

- JK BMS pack discharge (`src/energy.cpp`) is DC. It is published as `bms_discharge_wh` with `bms_discharge_kind = dc_pack_discharge`. It is not load consumption.
- ADS1115 A0/A1/A2 is the DC PV bus.
- Pinned main `src/inverter.cpp` parses STM32 UART1 (RX 33, TX 2, 9600) keys `VOUT` and `IOUT` only. `finalizeRecord` stores 0 after three misses. Those fields are not Vrms/Irms and have no validity bit or CT coverage, so the live estimate is unavailable (`stm32_text_vout_iout_not_rms`).
- Hardware v0.1.1 PZEM watt-hours are a different profile. They are measured watthours. This estimator does not accept them and they must not be multiplied by 0.8.

Coverage `unknown`, grid bypass while coverage is `inverter_output_only`, transfer, fault, and idle are not integrated. A valid 0 V or 0 A sample is a valid zero. A missing or invalid sample is not a zero.

## Identity and time

Identity is `device_id + stream + meter_epoch + boot_id + seq`. Reboot increments `boot_id` and does not reset the counter, epoch, or sequence. Clock adjustments do not reset them and do not rewrite saved records.

Civil checkpoints use Africa/Lagos (UTC+1, no DST) only while the DS1307 face and an external UTC source agree. `CH=0` and a plausible tick are not absolute trust. Stale NVS time is rejected. A DS3231 OSF bit is not read. An interval that crosses local midnight keeps its whole estimate on one record and is not split into the two days.

The DS1307, if fitted later, shares I2C GPIO 21/22 with the ADS1115 at 100 kHz. Electrical approval is still pending. The driver backs off and does not block inverter control. No wiring is performed by this firmware.

## Journal and upload

Records are appended to a dedicated SPIFFS journal and replaced only after a verified write. An open tail lost at reboot increments `loss_events` and is not reconstructed. A full journal of unacknowledged records sets `overflow_alarm` and does not overwrite them. HTTP success does not acknowledge a record. The server must commit the identity before the collector may retire it. That acknowledgement is not implemented in the current backend, so `transport` stays `pending_contract` and nothing is marked uploaded.

## Display

`/metrics` and `GET /load_ac_energy` carry the status object. The local pages show **AC estimate unavailable** until `estimate_available` is true and the attribution is `allocated` or `valid_zero`. They do not display `inverter_energy` or `bms_discharge_wh` as consumption.

`contracts/examples/SYNTHETIC_estimate.load_ac_energy.v1.json` is a labelled synthetic fixture for a parallel backend/app demonstration. It is not live telemetry.

## Backend PR 30

The counterpart draft is `letambari/official-powercell-app` pull 30, branch `cursor/estimate-ac-energy-v01-8522`, commit `e7f86c700cd930046f54e75dfec3998536953f54`, files `powercell-app/backend/contracts/estimate.load_ac_energy.v1.schema.json`, `powercell-app/docs/estimate_load_ac_energy_v1.md`, and `powercell-app/contracts/demo_fixture.php`.

Those files were not readable from this environment. `gh` returned “Could not resolve to a Repository”, and unauthenticated GitHub, `git ls-remote`, and page fetch all returned 404. No local copy was found. The collector JSON was therefore not renamed to match a summary of that schema. Guessed names would be an unverified wire format.

`python3 test/run_load_ac_energy_tests.py` validates this repo's host behavior. If `ACE_ENERGY_SCHEMA` points at the backend schema file, the same runner checks the synthetic fixture against that schema's required keys. Without the file, it reports the gap and does not claim compatibility.
