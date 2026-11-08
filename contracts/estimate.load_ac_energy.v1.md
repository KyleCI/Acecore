# estimate.load_ac_energy.v1

The wire record is the schema vendored from `letambari/official-powercell-app` commit `e7f86c700cd930046f54e75dfec3998536953f54`:

`contracts/upstream/e7f86c700cd930046f54e75dfec3998536953f54/`

SHA256 is in that directory's `SHA256SUMS`. The backend applies power factor 0.800 once when it checks `floor(Vrms_mV * Irms_mA * 800 * covered_ms / 3600000000000)` against `interval_estimated_mwh`. This firmware puts that product in the counter before send. The backend must not multiply by 0.800 again. `pf_kind` is `assumed` and `pf_assumption_millis` is 800. A different factor is not emitted.

`simulation` is false on collector records. The PHP fixture is synthetic (`simulation` true) and is not written into the journal or `/metrics`.

Identity for acknowledgement is `device_id + stream + meter_epoch + boot_id + seq + clock_correction_version`. The same stored body is a replay and is acknowledged again. A different body for that identity is a conflict and is not stored. HTTP success is not an acknowledgement. The attached files describe that commit-before-ack behavior; they do not include a separate response JSON schema.

A new `boot_id` restarts the counter for new records. Bytes already stored in the journal are not rewritten and are not rescaled.

## Live AC source

Pinned main still has no verified RMS source. `VOUT`/`IOUT` text is only a provisional inverter-only candidate when one `SYS : INVERTER` record contains both numeric fields. `SYS GRID` is not load coverage. `VoutValid` is not transmitted, `VOUT_SCALE_CALIBRATED` is 0, the IOUT note is a half-wave calculation with a 0.18 A clamp, and the archived sender has not been shown to be the installed STM32. The live estimator stays off. The status record uses `sample_validity` `absent`, `inputs` null, and `0` mWh.

The receiver no longer pairs an old `VOUT` with a later `IOUT`, does not treat three missed fields or a mutex failure as a real zero, and drops eligibility on boot, mode change, and grid switch. Further source that would still help: the exact `printf` of `VOUT`/`IOUT`, and confirmation that the installed image is banner `PC-CALEB-XFER4`.

Ingest described by the backend notes is `POST /api/telemetry/energy-estimate` or a device message `type` `energy_estimate`, and daily read `GET /api/energy/estimate/daily`. This firmware does not call those routes and does not claim they are deployed.
