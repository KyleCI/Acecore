# estimate.load_ac_energy.v1

The wire record is the schema vendored from `letambari/official-powercell-app` commit `e7f86c700cd930046f54e75dfec3998536953f54`:

`contracts/upstream/e7f86c700cd930046f54e75dfec3998536953f54/`

SHA256 is in that directory's `SHA256SUMS`. The backend applies power factor 0.800 once when it checks `floor(Vrms_mV * Irms_mA * 800 * covered_ms / 3600000000000)` against `interval_estimated_mwh`. This firmware puts that product in the counter before send. The backend must not multiply by 0.800 again. `pf_kind` is `assumed` and `pf_assumption_millis` is 800. A different factor is not emitted.

`simulation` is false on collector records. The PHP fixture is synthetic (`simulation` true) and is not written into the journal or `/metrics`.

Identity for acknowledgement is `device_id + stream + meter_epoch + boot_id + seq + clock_correction_version`, plus the stored `simulation` flag. The same stored body is a replay and is acknowledged again with the same `record_id`. A different body for that identity is a conflict and is not stored. The journal bytes, power-factor assumption, and identity are not rewritten to get past a conflict.

A new `boot_id` restarts the counter for new records. Bytes already stored in the journal are not rewritten and are not rescaled.

## Device-socket ACK

Backend commit `ab64d033ad4a0c03bde1bb62670e924d9cc460bf` forwards the commit-backed ACK on the existing `/ws/device` socket. The vendored schema, fixture, and socket forwarder are in:

`contracts/upstream/ab64d033ad4a0c03bde1bb62670e924d9cc460bf/`

Pending journal records are sent one at a time as `{"type":"energy_estimate","record":<exact stored object>}`. The wrapper is not written back into the slot. The frame carries no API key and no device secret. The socket keeps the existing `API-KEY` header and `device_id` query; this firmware does not add an authentication policy.

The record is retired only when the text frame is exactly the durable ACK: `status` `success`, `ack` boolean true, `replay` boolean, `contract` `estimate.load_ac_energy.v1`, `stream` `estimate.load_ac_energy`, `record_id` an integer greater than or equal to 1, and the same device, epoch, boot, sequence, clock-correction version, and simulation as the stored body. Retirement flips the slot only after the journal image persists. A failed persist leaves the original slot pending and the same bytes are sent again.

`{"status":"success"}`, an HTTP 200, a negative `ack: false`, `unavailable`, a model or identity conflict, a wrong stream, a non-integer `record_id`, or a malformed frame does not retire the slot. The diagnostic is a short token. The stored object is not dropped, relabeled, or resent under a new identity.

The journal device id remains the 12-hex form already stored in the 16-byte header field. It is not rewritten into the colon-separated MAC, and it is not case-folded, when an ACK echoes a different id. `device_mismatch` and any other negative frame leave the stored object unchanged and wait out a bounded backoff. Reconnect does not clear that wait and does not drop the record.

Host tests drive the session with in-memory frames and drive the journal commit steps with an in-memory directory. They do not open a socket, do not call `api.acecore.tech`, and do not power-cycle SPIFFS. A passing host fault test is not a hardware power-cut result. The SPIFFS adapter only compiles those same steps. This tree does not contain the backend database, so the commit-to-row step of `EnergyEstimateRepository::commitNew` was not run here. The stored id for ACE9678 is not known here and is not guessed.

## Live AC source

Pinned main still has no verified RMS source. `VOUT`/`IOUT` text is only a provisional inverter-only candidate when one `SYS : INVERTER` record contains both numeric fields. `SYS GRID` is not load coverage. `VoutValid` is not transmitted, `VOUT_SCALE_CALIBRATED` is 0, the IOUT note is a half-wave calculation with a 0.18 A clamp, and the archived sender has not been shown to be the installed STM32. The live estimator stays off. The status record uses `sample_validity` `absent`, `inputs` null, and `0` mWh.

The flashed `Powercell_caleb_xfer4` image contains banner `PC-CALEB-XFER4` and the formats `VOUT: %.1fV`, `IOUT: %.2fA`, and the closer `--------------`. The candidate source emits that report as three 20 ms ticks every 2 s, and prints `VOUT`/`IOUT` only for `SYS_INVERTER`. Rebuilding that source was not proven, so the live estimator stays off. The receiver closes a report on that exact closer. A following `SYS` closes the previous report as truncated and does not reuse its fields. `0.00A` remains the sender clamp, not a known no-load. `VOUT_SCALE_CALIBRATED` is 0 and the current path is a half-wave clamp, so a parsed record is not a calibrated RMS claim.

The live estimator stays off. `load_ac_energy_poll` does not call `on_sample`, and `MeterStatus.transport_enabled` stays false so a connected socket is not reported as a verified AC source. `SYS GRID` and bypass are not load coverage.

Ingest on the device socket is the `energy_estimate` frame above. This firmware does not call `POST /api/telemetry/energy-estimate` or `GET /api/energy/estimate/daily`, and it does not treat either HTTP status as a durable ACK. The installed STM32 image is still unidentified; the exact `printf` of `VOUT`/`IOUT` and proof that the installed banner is `PC-CALEB-XFER4` are still the missing source.
