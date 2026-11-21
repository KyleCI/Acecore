# AcePlug ESP-NOW socket firmware 1.1.0-espnow-event

**EVENT/DEMO BUILD ONLY. Not for customer units. Keys must rotate before customer builds.**

Caleb, 19 Nov 2026 16:19 WAT: "build an ESP-NOW plug image with the same keys and our 10 A trip
and 8 A warning". This image pairs with collector 2.5.40 (event build) over ESP-NOW. It uses the
same existing reference keys as 2.5.40, compiled in from the local, never-committed
`../include/espnow_pairing.h` (`POWERCELL_ESPNOW_EVENT_KEYS=1`). Built bins contain those keys.

## What it is
- Base: reference socket 1.0.1 (`smart-socket-firmware/src/main.cpp` in "Col PL.zip"): channel
  scan for the collector beacon, discover every 1 s, telemetry every 5 s, Pair/PairAck,
  Forget/ForgetAck, PMK/LMK encrypted unicast, PowercellOTA, OTA relay hold-off, 15 s watchdog.
- Wire format: `../lib/pc_core/src/espnow_wire.h`, the same header compiled into collector
  2.5.40 (44-byte v1 packet, HMAC-SHA256/16 with the AUTH key). No private protocol copy.
- Safety from the LoRa AcePlug firmware (537d7f0 lineage, 1.0.3), replacing the 50 A trip:
  - trip at >= 10.0 A: relay off at once, auto reclose after 15 s
  - 3 trips within 5 min: lockout, relay latched off, saved in NVS (`lockout`), survives reboot;
    clear by holding the BOOT button (IO0) for 3 s (relay stays off; switch it on from the app)
  - warning at >= 8.0 A, clears below 7.0 A (serial log `[SAFETY][CURRENT_HIGH]`)
  - RELAY_ON refused while tripped, locked out or during OTA (telemetry "rejected" bit)
  - current: ZMCT103 RMS with 0.70 A idle noise removed in quadrature, 0.15 A floor, EmonLib
    settled at boot, calibration 5.1
- What collector 2.5.40 sees: trip and lockout both appear as the v1 "overcurrent" telemetry
  flag. The 8 A warning and lockout detail go out in an optional Status packet (type 9) that
  2.5.40 decodes and ignores, so in the app the warning is not shown yet. SET_LIMIT and
  RESET_LOCKOUT from the app answer "unsupported" on ESP-NOW (the limit is fixed at 10 A).

## Pins (AcePlug v0.1 CAD, same as the LoRa plug firmware)
| Function | GPIO |
|---|---|
| Relay driver | IO15 |
| Status LED | IO4 |
| ZMCT103 CT (ADC1) | IO35 |
| Reset / BOOT button | IO0 |
| RFM95 NSS (radio unused, held HIGH) | IO5 |

No conflicts with the reference socket (LED 4, relay 15, current 35). The RFM95 stays fitted and
idle; RST IO25 / DIO0 IO26 / SPI 18/19/23 are not driven.

## Build (keys never leave the local file)
1. Put the reference `espnow_pairing.h` at `../include/espnow_pairing.h` (repo root `include/`,
   gitignored, same file the 2.5.40 build uses). The `.example` placeholder fails the build.
2. `cd aceplug-espnow-firmware && pio run`
3. Without `POWERCELL_ESPNOW_EVENT_KEYS=1` the build stops: customer key provisioning is not
   implemented in this plug yet.

## Flash and pair: see FLASH_README.txt in the USB zip.
