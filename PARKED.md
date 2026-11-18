# PARKED / WIP: ESP-NOW smart socket + V x I power (branch cursor/espnow-socket-2536)

Parked 18 Nov 2026 (WAT) on Caleb's direction: LoRa stays the live smart-socket link,
ESP-NOW stays off, no V x I work for now. Based on ad90eb0 (2.5.35, JBD). Not built,
not tested on hardware, not released. Do not merge.

## Done (source only, nothing wired into the build yet)
- `lib/pc_core/src/espnow_wire.h`: the 44-byte v1 ESP-NOW packet, byte-compatible with
  collector 1.0.4 / socket 1.0.1, signed with pc::hmacSha256 (no mbedtls). Adds optional
  Status (9) and Control (10) types for lockout / set-limit. Also has the encrypted-peer
  budget helper (7 encrypted peers, at most 6 paired sockets).
- `lib/pc_core/src/socket_power.h`: P = collector output V x socket I, labelled as apparent
  power (VA). 0.1 A noise floor. Null when the voltage is stale. Key list for the compact
  /data snapshot.
- `src/sockets/socket_power_json.cpp` + declarations in `src/sockets/sockets.h`: voltage
  read (PZEM on DSP, STM32 vout on STM) and the JSON keys `voltage`, `voltage_source`,
  `apparent_power_va`, `power_quantity`, `current_usage_watts` (= VA).
  Nothing calls these yet.

## Not done
- `sockets_espnow.cpp` rewrite: LoRa-path parity, peer cleanup, 4096 stack / queue 8.
  The file still includes the missing `espnow_keys.h`, so `FEATURE_ESPNOW_SOCKETS=1`
  does not compile yet.
- Calls from `sockets_lora.cpp` / `sockets_espnow.cpp` to `socketPowerToJson`.
- The web_local compact /data snapshot using `kCompactSnapshotKeys`.
- DSP guards (`backend_ws.cpp:281`, `web_local.cpp:524`). Parking the STM SX1278.
- platformio.ini flag, the 2.5.36 version bump, docs and CHANGELOG.
- `smart-socket-firmware-espnow/`: keys from NVS, 10 A trip / 8 A warn / lockout.
- Host tests (socket_power, ported ESP-NOW decoder cases), builds, Drive delivery.
- Open questions: ESP-NOW key provisioning (factory.py), backend ingest of the new
  socket keys, router channel changes and the heap margin against the 51,200 B TLS gate.

Warnings and trips stay current-based (amps). VA overstates real watts for
non-resistive loads.
