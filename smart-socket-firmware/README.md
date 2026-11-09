# smart-socket-firmware (Acecore 2.2 edits of Dubem's socket firmware)

Edited copy of `acecore-collector-dubem/smart-socket-firmware/src/main.cpp`.
It builds inside Dubem's tree (it needs `../include/powercell_lora_profile.h`
and `../lib`): copy `src/main.cpp` over the original and run `pio run`.

Changes:
- Oct 26 2026: AcePlug fitted relay is **10 A**. Trip at 10 A, warn at 8 A (clear below 7 A).
  User SET_LIMIT clamped to 1..10 A. (Was 20 A / 16 A on Oct 22; earlier `OVERCURRENT_AMPS` = 50 A.)
- Oct 22 2026: trip/warn thresholds, immediate telemetry on trip or warning change,
  removed the debug `toggleRelay()` that ran on every received packet.
Unchanged: the 15 s auto-restore after a trip, plaintext unauthenticated protocol.
