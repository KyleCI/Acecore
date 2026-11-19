# ESP-NOW smart sockets (firmware 2.5.40)

Caleb, 19 Nov 2026 14:31 WAT: "activate the Espnow and deactivate Lora". All six images
(pc-dsp011-15kw/35kw/5kw, pc-stm01-15kw/35kw/5kw) talk to the smart sockets over ESP-NOW.
LoRa is compiled out; its code stays in the tree.

## Reference
Caleb's "Col PL.zip": the collector 1.0.4 / socket 1.0.1 ESP-NOW tree (collector `src/lora.cpp`
is the ESP-NOW transport behind the old LoRa API names, `include/espnow_protocol.h` the wire
format, `smart-socket-firmware/src/main.cpp` the socket, `test/ESPNOW_VALIDATION.md` the
4 Oct 2026 close-range test). The zip also holds `include/secrets.h` and
`include/espnow_pairing.h` with real values. Nothing from those two files is in this tree.
Treat the keys in them as exposed and rotate them before any rollout.

## What 2.5.40 takes from it
- The 44-byte v1 packet, HMAC, types 1..8, flags, pairing / forget acceptance rules, beacon 1 s,
  command retry 750 ms x 6, online = heard within 20 s, pair window 10 s. Byte-compatible with
  socket 1.0.1 (`pc_core/espnow_wire.h`, from the parked 907b9b4).
- Peers on channel 0 (current channel), station interface, PMK + LMK encrypted unicast,
  unencrypted broadcast beacon, Wi-Fi power save off.

## What differs from the reference collector
- Keys from NVS (`SET espnow_keys`), not from a compiled header.
- The 2.5.x app API on top (cmd_id, desired / pending, supersede, confirmed-from-telemetry,
  change push, VA power keys, alert key), the same as the LoRa path.
- Encrypted-peer budget (7) with eviction of idle unpaired peers; bounded device map.
- Paused during local OTA; modem sleep only in the battery low-power latch.

## Bring-up (Caleb, per unit; nothing here is done by the agent)
1. Generate one fresh fleet key set: 48 random bytes = PMK 16 + LMK 16 + AUTH 16, as 96 hex.
   Keep it out of git and chat.
2. Collector: before LOCK, over USB serial: `SET espnow_keys <96 hex>` then `REBOOT`
   (or put `ESPNOW_KEYS=` in factory_state/factory.env for factory.py / factory_flash.py).
   A LOCKED unit refuses it until FACTORY_WIPE.
3. Sockets: the AcePlug must run ESP-NOW socket firmware built with the same PMK / LMK / AUTH.
   The LoRa AcePlug firmware cannot talk to this collector.
4. Boot log: `[ESPNOW][READY] collector=<MAC> channel=<n> encrypted_unicast=1`.
   `[ESPNOW][NO_KEYS]` / `[ESPNOW][SOCKETS_OFF]` = keys missing.
5. Pair from the app (Add socket) within 10 s of the socket being heard.

## Open items
- Socket 1.0.1 trips at 50 A (`OVERCURRENT_AMPS`); the AcePlug relay is 10 A. An ESP-NOW socket
  build needs the 10 A trip / 8 A warn of the LoRa AcePlug firmware before use.
- Not tested on hardware: range through walls, router channel change, OTA coexistence, heap
  against the TLS gate with ESP-NOW running.
