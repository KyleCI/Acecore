# AcePlug smart-socket firmware 2.5.0 (v0.1)

Pins below are **CAD-verified v0.1** from Smart Socket.kicad, U3 ESP32-WROOM-32. v0.2 is deferred and is not in this image.

Build from this directory:

```bash
pio run -e esp32dev
```

This image drives the CAD-verified v0.1 map (RELAY IO15, ZMCT103 CT on ADC IO35 through EmonLib). Do not flash a v0.2 board with it (RELAY 27, CS 32, RST 26, DIO0 34, BL0942 UART IO16/IO17). Do not use the GPIO14/4/2/33/22/21 list in `LORA_SOCKET_PROTOCOL.md`. QGatePass/Acecore `main` stays untouched until the collector and this socket both build on KyleCI PR #1.

There is no fixed `upload_port`. Pass `--upload-port` only when the board in front of you is v0.1.

Radio settings come from `include/powercell_lora_profile.h` (same profile as the collector):

| Setting | Value |
|---|---|
| Frequency | 433 MHz |
| Spreading factor | 9 |
| Bandwidth | 125 kHz |
| Coding rate | 4/5 (`setCodingRate4(5)`) |
| Preamble | 12 |
| Sync word | `0xF1` |
| CRC | on |
| TX power | 20 dBm |
| SPI clock | 2 MHz |

v0.1 pins in `src/main.cpp` (CAD-verified on Smart Socket.kicad U3; not the collector header and not v0.2):

| Define | GPIO |
|---|---|
| `RELAY_PIN` | 15 |
| `LORA_CS_PIN` | 5 |
| `LORA_RESET_PIN` | 25 |
| `LORA_DIO0_PIN` | 26 |
| `LORA_SCK_PIN` / `LORA_MISO_PIN` / `LORA_MOSI_PIN` | 18 / 19 / 23 |
| `LED_PIN` | 4 |
| `CURRENT_PIN` | 35 (ZMCT103 CT, EmonLib, not BL0942) |
| `RESET_BUTTON_PIN` | 0 (module BOOT button) |

`RELAY_PIN` is SOCKET_RELAY. Receive is polled. IO15 is driven low at the start of `setup()` because it is a strapping pin. Current is the ZMCT103 CT through EmonLib on ADC 35. BL0942 belongs to v0.2 and is not compiled.

The schematic symbol is RFM95W-915S2. This image is 433 MHz, sync `0xF1`, SF9. The module on the board must be a 433 MHz part. A 915 MHz module will not hear the collector.

The fitted contact is 10 A. Software protection warns at 8 A (clears below 7 A) and trips at 10 A. User limits are 1.0–10.0 A. This firmware does not describe a 20 A AcePlug.

Commands from the collector: `<id>,RELAY_ON,<cmdId>`, `<id>,RELAY_OFF,<cmdId>`, `<id>,RESET_LOCKOUT,<cmdId>`, `<id>,SET_LIMIT,<deciamps>,<cmdId>`. Command ids are 1–47 characters of `[A-Za-z0-9-]`.

Telemetry: `<id>,SOCKET CURRENT:<A>,SOCKET RELAY:<0|1>,FW:<ver>` plus `ACK`, `LIM`, `WARN:CURRENT_HIGH`, `ALERT:OVERCURRENT`, and `ALERT:LOCKOUT` after 3 trips in 5 minutes.

`trip_lockout.h` is the lockout used by this image and by the collector host tests.

OTA still uses `../lib/PowercellOTA` and `../include/websocket_ca.h`. Kind is `socket`, version `2.5.0`. Do not put `include/secrets.h` or `.pio/` in a source zip.

A board populated with the 915 MHz module drawn as RFM95W-915S2 will not hear this collector. Fit a 433 MHz module and build this image.
