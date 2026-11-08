# Auth provenance and latch-recovery evidence

This note records what is in this repository. It does not treat deployed
firmware 2.5.11 as this tree, and it does not complete Manual Save.
`GET` and `POST /v1/battery-settings` stay unsupported: a bearer token is
rejected with HTTP 401, `authenticated_configuration` false, and
`settings_save_complete` false. No new login, token, or credential check
was added.

## Why this branch has no LAN token

PR #2 is `cursor/hardware-v0.1.1-21ad`, branched from `main` at
`a0932adec88a1f5ee3679becf51a084874f986f1` ("Added Base files"). That
collector is the 1.0.1 tree. Searched sources on this branch: there is no
`/v2/login`, no `lan_token`, and no inbound bearer check. `Authorization:
Bearer` appears only as the collector's outbound OTA client header.

The only other remote branch is `origin/embedded/collector-v17-app-ready`
at `55b18aa34e1f292b9c30429d29475db82c85f27c`. `git merge-base` with this
branch is the same `a0932ad`. Its README and `include/config.h` name the
firmware **2.5.0**, not 2.5.11. Commit `4f39c95` is not an object in this
clone.

That 2.5.0 branch is the only source here that implements the app's local
login:

- `src/web_v2.cpp` registers `POST /v2/login`. The body carries `device_id`,
  `passcode`, `phone_id`, and `phone_name`. On success it returns `session`
  (`expires_in` 1800), `lan_token`, and `passcode_default`.
- `onboarding::openSession` issues a new random session and a new random
  LAN token for that phone and stores the pair. A later login for the same
  phone replaces that phone's token.
- `onboarding::authorize` accepts `Authorization: Bearer` when the value
  matches a live session or a stored LAN token. Anything else is
  unauthenticated. There is no admin-token fallback in that function.
- `lib/pc_core/src/local_auth.h` is a different piece. Its
  `BatteryAlertPolicy` is the display policy at 20 / 10 / 5 with 3 points of
  hysteresis. It also has Basic-auth helpers. It does not issue or check
  `lan_token`.

`web_v2.cpp` is not a drop-in file. The login route calls onboarding
(passcode verifier, session table, pair store), settings (device id and
device secret), and the local HTTP server. The same translation unit also
registers socket config, time, passcode change, pair, and factory routes.
The branch diff against `a0932ad` is 506 files. It adds `src/stm32_link.cpp`,
`src/power_guard.cpp`, a different OTA client, and AcePlug socket rules, and
it removes the 1.0.1 web UI. Copying that branch in would replace this
collector, including the v0.1.1 pin map.

## What a later auth integration would have to be

Authenticated Manual Save is required for the existing app. It is not
optional. It is also not done on this branch.

A safe follow-up would extract only the 2.5.0 bearer check and the
`POST /v2/login` session/LAN-token issue path, then point
`/v1/battery-settings` at that check. It would need the onboarding store
those functions already use (passcode verifier, sessions, phone pairs) and
the device id those functions already compare. It would not add a second
identity system.

That extract is not safe to do from the files as they sit, because the
login translation unit and the power-guard translation unit pull in STM32
gateway control, socket shedding, and 2.5.0 OTA. Those behaviors are
unrelated to a battery-settings save. The missing material is a 2.5.11
tree at `4f39c95`, or a written confirmation that the 2.5.0 login above is
the one the app already calls. Until one of those is on hand, this
endpoint stays rejected.

No live device was contacted and no credential was read.

## JK `batStatus` in this tree

`src/bms.cpp` stores dynamic byte `f[172]` in `batStatus`. The printer
labels 0 Standby, 1 Charging, 2 Discharging. `include/bms.h` describes the
same byte as charging, discharging, or idle. The 2.5.0 parser
(`lib/pc_core/src/jk_bms.h` on the other branch) documents the same byte
as "0 standby, 1 charging, 2 discharging (as sent)". Neither parser
invents a fourth "full" state.

Pack current is dynamic bytes `f[158..]` . This tree's `energy.cpp` treats
negative current as discharge and ignores non-negative current. The 2.5.0
parser documents that same integer as milliamps, positive when charging.
A resting full pack is standby (`batStatus` 0) with current near zero. It
is not `batStatus == 1`.

## Recovery options

The requested restart gate is fresh SOC at or above 15%. This branch
currently requires more than that, and that extra requirement is a decision
still owed. It was not widened or narrowed again in this pass. No stale-SOC
shutdown and no automatic turn-on were added.

Host test `fullIdleAboveFifteenStaysLatched` holds SOC at 100% with
`charging == false` for ten times the 60-second window. The latch stays
closed and `outputOnAllowed()` stays false.

| Option | Evidence | Full or idle pack above 15% | Status |
| --- | --- | --- | --- |
| A. Current rule | This branch. Release only if every fresh sample for 60 seconds has SOC >= 15 and `batStatus == 1`. | Stays latched. Standby is not charging. | Implemented. Narrower than "restart at 15%". |
| B. 2.5.0 `soc_guard.h` | `origin/embedded/collector-v17-app-ready` `lib/pc_core/src/soc_guard.h`. Release when SOC >= 15 and charging has been held 60 seconds. Charging means BMS online and current > +1 A, or, if the BMS is offline, STM32 BULK/ABSORB/FLOAT. The host test rejects a 0.5 A dip. | Stays latched while current is at or under 1 A. | Not adopted. The STM32 fallback does not exist on v0.1.1. `power_guard.cpp` on that branch also calls `gateway::set(true, "soc_restore")` after release. That automatic turn-on must not be copied. |
| C. BMS online and SOC >= 15 | Described for deployed 2.5.11 audit `4f39c95`. That commit is not in this repo. The readable 2.5.0 guard does not release on BMS-online plus SOC alone. | Would release a resting full pack if the audit description is exact. | Not implemented. Needs the 2.5.11 source, or an explicit confirmation, before it can replace option A. |

Option A remains in force so this pass does not pretend a resting pack has
met a charging hold. It does leave a unit that charged past 15% and then
went full or idle latched until `batStatus` returns to 1 for a full minute.
Embedded needs to choose among A, B without the STM32 fallback and without
automatic turn-on, or C once the 2.5.11 source is available.

## Physical output

`BatteryAlertPolicy::kPhysicalAutoOffEnabled` is false. The latch still
blocks collector ON. The new edge that used to call `applyDeviceState(false)`
at 10% is consumed and does not drive a pin. On the original profile that
call writes relay GPIO15 and the light. On v0.1.1 it writes only the GPIO15
power light. Neither write is a verified discharge cutoff, so the new
automatic write stays off. Pre-existing `/OFF`, websocket OFF, boot relay
init, and the OTA shutdown callback are unchanged. v0.1.1 GPIO15 remains
the power light. `physical_shutdown_verified` stays false. No BMS FET write
and no DSP wiring were added.
