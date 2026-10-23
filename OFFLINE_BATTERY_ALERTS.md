# Offline BMS battery alerts

This implementation is in the active LoRa collector build. The archived
`src/webinterface.cpp` is still excluded by `platformio.ini` and is not used.
No backend connection, Firebase, internet access, or synchronized clock is
required to generate these local notifications.

## Rules

| State | Entry threshold | Exit threshold | Reminder interval |
| --- | --- | --- | --- |
| Low | SOC <= 25% | SOC > 27% | 2 hours |
| Critical | SOC <= 10% | SOC > 12% | 1 hour |
| Shutdown risk | SOC <= 5% | SOC > 7% | 30 minutes |

Escalation is immediate; recovery uses a 2 percentage-point margin to avoid
repeated alerts from fluctuating readings. Only one battery-level alert is
active. A reminder occurs only when a valid reading remains in that state.
These alerts do not operate relays, shed loads, or shut down the collector.

The BMS reader timestamps accepted frames. An alert reading needs a complete
dynamic frame with the expected header/type, plausible cell/pack data, SOC in
0..100, positive pack voltage, and an age no greater than 15 seconds. Existing
parser offsets are retained; no new checksum algorithm is assumed. A fresh,
valid 0% is a critical reading, while missing data is not treated as 0%.

There is a 15-second startup grace period. Stale/invalid data clears the
previous level alert and raises one `battery_reading_unavailable` warning.
Fresh data clears that warning and re-evaluates SOC. Repeated invalid samples
do not repeatedly generate notifications.

## Active source files

- `include/battery_alert_policy.h`, `src/battery_alert_policy.cpp`: portable
  threshold, hysteresis, freshness, and cooldown policy.
- `include/bms.h`, `src/bms.cpp`: BMS timestamp and plausibility checks.
- `src/offline_battery_notifications.cpp`: JSON snapshots and SSE delivery.
- `src/main.cpp`: initialization before HTTP start; evaluation from `loop()`.

## Local HTTP contract

- `GET /data`: uncached JSON with `device_id`, `device_state`, a `battery`
  object (`valid`, `status`, `percentage`, `voltage`, `age_ms`), and
  `notifications` containing both latest alert records, including inactive ones.
- `GET /events`: SSE `ready`, `state`, `notification`, and `heartbeat` events.
  On connect, the current state and both notification records are replayed.
  State/heartbeat messages are sent at least every 10 seconds while the main
  loop runs normally. Policy evaluation is once per second, outside HTTP tasks.

Every alert contains `collector_id`, `id`, `type`, `severity`, `title`,
`message`, `active`, `sequence`, `sequence_persistent`, `sound`, and `dummy:false`.
The battery-level alert always uses `id:"battery_low"` even when its type
changes to `battery_critical` or `shutdown_imminent`. This lets the phone replace
and cancel the same notification rather than leaving several active.

Sequence numbers reserve a range in NVS once per boot, fit within JavaScript's
safe integer range, and increase on transitions/reminders. Repeated reads of
`/data` do not advance them. If NVS fails, a logged random boot namespace is
used with `sequence_persistent:false` instead of claiming monotonic ordering.

The companion Flutter app now prefers the event's `collector_id` and ignores
out-of-order sequences for `sequence_persistent:true`. This prevents initial
connection duplicates and delayed snapshots resurrecting cleared alerts.

## Verification

Run the production policy on a host machine:

```sh
python3 test/run_battery_alert_tests.py
```

Compile without flashing:

```sh
pio run -e esp32dev
```

Tests cover threshold boundaries, recovery margins, invalid/absent/stale
readings, reconnection, reminder cooldowns, valid 0%, and millis rollover.
The app's `test/offline_battery_notification_test.dart` covers replay,
replacement, cancellation, out-of-order snapshots, reboot sequences, and dummy
suppression with the notification platform mocked.

After flashing the collector and installing the matching app, inspect
`http://<collector-ip>/data` and keep the app's Offline screen open. Use a BMS
simulator/test setup to exercise thresholds rather than discharging a battery
to unsafe levels. Verify that disconnecting BMS data raises unavailable after
15 seconds and recovery removes the old phone alert. A firmware compile does
not validate the physical BMS link or notification permissions on a phone.

The current Flutter offline listener pauses when the app backgrounds. This
change does not promise alerts with the app closed or suspended.
