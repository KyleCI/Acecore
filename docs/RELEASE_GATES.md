# Release gates

This branch is not a finished release. Software that is already tested is
listed separately from items that still need a person, a secret, or a bench.

## Ready in software, host-tested, not run on a unit

- `POST /v2/login`, `GET /v2/setup/info`, `GET /v2/status`.
- `POST /v1/system-rating` for rated active watts 5000 or 3500. Usable power
  is computed, 4500 W or 3150 W, and is the overload bound. A lower Manual
  ceiling cannot raise it. On v0.1.1 a sustained fresh reading above the bound
  requests GPIO15 high. That does not turn the output back on, and it is not
  a separate hardware breaker.
- Local Manual Save for battery percentages and the AC ceiling, with
  `result: applied`, `applied: true`, `durable_commit: true`, and a later GET
  of the same revision and values. A login session is
  `session_not_sufficient` on both settings routes. `API-KEY` and an OTA
  bearer are `unauthorized`.
- `POST /state` for the collector output. v0.1.1 uses GPIO15 active-low.
  The original profile uses its existing relay. `POST /relay` is AcePlug over
  LoRa and, on v0.1.1, returns `lora_not_fitted` without writing GPIO15.
- Battery latch at 10 percent drives v0.1.1 AC output off and does not turn
  it back on at 15 percent. BMS FET registers are not written.
- DS3231 validity checks. The module identity is user-confirmed, not measured.

## v0.1.1 requirements still blocked

- PZEM placement and calibration. The software path runs only on a fresh
  valid configured reading, and `physical_shutdown_verified` stays false
  because the meter has not been shown to be the total AC output.
- Payment-lock setter. 2.5.11 sets `pc_ident/pay_lock` from a signed cloud
  command, HMAC of the factory device secret. This tree reads the flag and
  refuses user on and off when it is already true. Battery cutoff and OTA off
  still run. An unsigned local setter would weaken that rule, so it is not
  added. The secret is not in the review archive.
- Device timing. The app allows 12 seconds for headers plus body and does not
  replay a timed-out POST. PBKDF2 is intended to finish inside that, but it
  has not been timed on an ESP32. A login that succeeds on the device and is
  then lost still replaces that phone's token.
- Physical confirmation. Charging-continues and DSP polarity are user
  statements, not measurements. GPIO15 high as AC-output off is the same.

## Original profile

The 19 A rule is also not copied there. The text parser stores a field named
`iout`, and that field is not shown to be the 2.5.11 RMS current. The
original relay mapping is unchanged. LoRa socket relay remains the original
path. v0.1.1 DSP behavior is not applied to that relay.

## What would close each gate

- The signed payment-lock command and its key, held outside this archive, if
  the lock must be set from the cloud again.
- One timed login on hardware, and one check that a lost response is recovered
  by logging in again rather than by replaying the POST.
- A bench check that GPIO15 high stops only AC output, and that the PZEM is
  on the total AC output and calibrated.
