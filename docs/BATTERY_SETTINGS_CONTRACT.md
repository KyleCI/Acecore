# Battery settings contract (collector firmware)

This document is the executable contract for the collector source in
KyleCI/Acecore, branch `cursor/hardware-v0.1.1-21ad`. It is not a description
of deployed firmware 2.5.11 and it was not checked by posting to a live device.

## Provenance

Searched this tree for `lan_token`, `/v2/login`, and an inbound
`Authorization: Bearer` check.

- There is no `/v2/login` and no stored or rotating per-phone LAN token.
- `Authorization: Bearer` appears only in `lib/PowercellOTA/src/PowercellOTA.cpp`,
  where the collector is the client and the bearer value is the device OTA
  token sent to the cloud. That token is not a LAN token and is not accepted
  here as one.
- The only inbound local header check in this tree is `API-KEY` against NVS
  `powercell_ota` / `api_key` on `GET /diag/export`, and only in the v0.1.1
  build. Battery settings do not accept that header. Using it would be a
  different credential than the app's LAN token.
- The cloud websocket authenticates the collector to the server with that same
  API key. It is not a phone LAN session.

Deployed 2.5.11 (audit baseline `4f39c95`) is a different artifact. This
contract does not claim compatibility with it. In that audit, per-plug
`SET_CONFIG` shed keys are not this battery policy, and that artifact had no
configurable low/warning endpoint. Those plug settings are not implemented or
acknowledged here.

## Endpoint

On the collector HTTP server (port 80), reached on the Powercell AP or LAN.
No cloud hop.

| Method | Path | Body |
| --- | --- | --- |
| GET | `/v1/battery-settings` | none |
| POST | `/v1/battery-settings` | JSON, ignored until a LAN token exists |

Header the app already sends:

```http
Authorization: Bearer <lan_token>
```

There is no other supported scheme. `API-KEY` is not accepted on this path.

## Save status

Manual Save is not complete. The app requires `Authorization: Bearer
<lan_token>` from `POST /v2/login`. This branch does not implement that
login. HTTP 401 here is the blocker, not an applied configuration.
`authenticated_configuration` and `settings_save_complete` are both false.
Provenance and the later integration boundary are in
`docs/AUTH_AND_RECOVERY_EVIDENCE.md`.

## What this firmware does today

`classify_lan_authorization()` never returns success. A missing, empty, or
non-bearer header is `unauthorized`. A non-empty `Bearer` token is recognized
and rejected as `lan_token_unsupported`. No NVS write happens. The live
thresholds stay at the defaults. Both methods respond **401** with:

```json
{
  "schema": 1,
  "result": "rejected",
  "reason": "lan_token_unsupported",
  "applied": false,
  "durable_commit": false,
  "pending": false,
  "physical_shutdown_verified": false,
  "physical_auto_off": false,
  "authenticated_configuration": false,
  "settings_save_complete": false,
  "output_authority": "software_inhibit_only",
  "collector_profile": "hw-v0.1.1 or original",
  "collector_source": "KyleCI/Acecore",
  "lan_token_issued": false,
  "deployed_2_5_11_compatible": false,
  "readback_present": false
}
```

`output_authority` is `software_inhibit_only` on both profiles.
`physical_auto_off` is false: the latch does not drive a pin.
`physical_shutdown_verified` is always false. v0.1.1 GPIO15 is the power
light and is not a discharge cutoff.

`pending` is always false. This firmware does not queue a save.

Save classification for the app:

- Body parses as schema 1 and `result` is `rejected`: rejected. Do not treat the
  requested numbers as applied.
- Body parses as schema 1 and `result` is `applied` and `durable_commit` is
  true and `readback.low_percent` / `readback.warning_percent` equal the
  request and `readback.applied_revision` is the new revision: applied.
- Anything else, including a generic HTTP 200, a generic ACK, a timeout, or a
  body without `result`: unknown. Do not mark the save applied.

This firmware does not return `applied` today, because it cannot check
`lan_token`.

## Rules the host-tested store already enforces

`BatterySettings::set` is what a future authenticated call would run, in this
order. A failure leaves the previous canonical state unchanged.

1. Authorization. `LanTokenUnsupported` and any non-ok auth reject before the
   body matters.
2. `schema` must be `1`. Any other value is `unsupported_schema` (HTTP 400 once
   auth exists). The response is not a generic 200.
3. `low_percent` and `warning_percent` must be integers. `not_integer` otherwise.
4. Ordering, so a warning cannot be presented as the hard cutoff:
   - inhibit is fixed at **10** and is not writable
   - restart eligibility is fixed at **15** and is not writable
   - `warning_percent` must be an integer **11..80** (strictly above 10)
   - `low_percent` must be an integer **12..95** and strictly greater than
     `warning_percent`
   - `warning <= 10` is `warning_not_above_cutoff`
   - `low <= warning` is `low_not_above_warning`
   - other range misses are `out_of_range`
5. `base_revision` must equal the current `applied_revision` (0 before the
   first successful save). A lower or repeated revision is `stale_revision`.
   The stale request is not applied. The client must read back and retry with
   that revision.
6. The NVS write must succeed. On failure the reason is `persist_failed`,
   `durable_commit` is false, and the live policy is not changed.
7. Only then does `applied_revision` increment by one, the new percentages
   become canonical, and the response includes `readback`.

A successful body, once auth exists, would be HTTP 200:

```json
{
  "schema": 1,
  "result": "applied",
  "reason": "none",
  "applied": true,
  "durable_commit": true,
  "pending": false,
  "physical_shutdown_verified": false,
  "readback": {
    "schema": 1,
    "low_percent": 30,
    "warning_percent": 18,
    "inhibit_percent": 10,
    "restart_percent": 15,
    "applied_revision": 1,
    "durable": true,
    "physical_shutdown_verified": false,
    "charging_evidence_required": true
  }
}
```

Defaults before any applied save: low 25, warning 15, revision 0.

## AC output ceiling

The selected measurement on v0.1.1 is the PZEM-004T V3 active-power register
`power_dW` (0.1 W per count), published as `ac_consumed.active_power_w`.
Freshness is 2 seconds. A missing or stale frame is unavailable. It is not
reported as 0 W and it does not start or clear a shutdown. BMS watts, solar
watts, V times I, and plug sums are not this input. The original profile has
no PZEM, so it has no selected source.

`total_output_coverage_verified` is false: placement on the inverter output
and calibration are not confirmed. Warn-and-shutdown does not drive a pin.
`physical_shutdown_verified` stays false. A saved ceiling is not applied on
the device until bearer login exists; the host-tested store accepts an
integer ceiling from 50 to 20000 W and mode `warn_only` or `warn_and_shutdown`.

## Latch recovery (not the configurable warning)

The configurable warning does not move the 10% latch or the 15% restart gate.

Release requires every sample in a 60-second window to be a fresh JK-BMS frame
whose own SOC stays at or above 15%, including a full or idle pack
(`batStatus` standby). A percent that is not from that BMS frame does not
release. Stale frames reset the window and do not release. Release does not
turn the output on. The 2.5.11 `soc_guard.h` rule this follows is BMS-online
with its own SOC >= 15 held 60 seconds. Charging current is not required for
that path.

This tree has no `soc_guard` module. The charging flag is the BMS evidence
available here. Pack-current sign is not used, because this firmware treats
non-negative current as "not discharge", which includes idle.

The one-time shutdown warning rearms after fresh SOC stays above the
configured warning percent for 60 seconds. That rearm does not require
charging and does not clear the latch. Changing the saved warning level does
not by itself emit another warning in the same episode.

Ambiguity: if the saved warning is below 15, the warning can rearm while the
latch is still held, because restart eligibility stays 15. The warning text
still says the output turns off at 10% and stays off until 15%.

## Physical authority

| Profile | What firmware can do at 10% | What it cannot do |
| --- | --- | --- |
| Original | Reject its own ON command. The new latch does not drive GPIO15. | Command the STM32, write BMS FETs, or treat the collector relay as a verified discharge output |
| v0.1.1 | Reject its own ON command. GPIO15 stays the power light and the latch does not drive it. | Command the prebuilt DSP. There is no STM32 or LoRa link. |

`physical_shutdown_verified` stays false in every response. No payment-lock
actuator exists in this firmware. Smart-socket ON is a separate path and is
not this battery setting.
