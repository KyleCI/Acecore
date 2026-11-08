# Hardware v0.1.1 collector

Hardware v0.1.1 is a board profile, not a firmware version. The firmware
version string stays `1.0.1`. The v0.1.1 build reports hardware model
`hw-v0.1.1` so an OTA job for `esp32dev` does not match it. Do not publish an
OTA job for `hw-v0.1.1` until the bench below has been run. This change does
not add an OTA feature and nothing here is flashed or deployed.

Two PlatformIO environments:

| Environment | Profile | What it keeps |
| --- | --- | --- |
| `esp32dev` (default) | Original | UART1 STM32 text on RX GPIO33 / TX GPIO2, LoRa SPI, light GPIO25, relay GPIO15 |
| `esp32dev_hw_v011` | v0.1.1 | No STM32 and no LoRa. Sensors and the SD serial log only |

Build them with `pio run -e esp32dev` and `pio run -e esp32dev_hw_v011`.
Host checks, with no board attached:

```bash
python3 test/run_hw_v011_tests.py
python3 test/run_battery_alert_tests.py
```

## UART allocation

Classic ESP32 has three hardware UART controllers. v0.1.1 uses all three.
There is no software UART.

| Controller | Original | v0.1.1 |
| --- | --- | --- |
| UART0 | Console TX GPIO1 / RX GPIO3, 115200 | Same console |
| UART1 | STM32 text RX GPIO33 / TX GPIO2, 9600 | PZEM-004T V3 Modbus RX GPIO25 / TX GPIO26, 9600 8N1 |
| UART2 | JK-BMS RX GPIO17 / TX GPIO16, 115200 | Same BMS link |

PZEM defaults are `PZEM_RX_GPIO` 25, `PZEM_TX_GPIO` 26, address `0xF8`,
timeout 200 ms. The pins can be changed at build time. A pin that collides
with the console, the BMS UART, I2C, the power light, or the SD chip select
is rejected at compile time. Swapping RX and TX still builds, because both
pins are the PZEM pair; the meter has to answer on the bench before any
reading is trusted.

GPIO2 and GPIO4 are released on v0.1.1 (old STM32 TX and old LoRa CS). They
are not assigned. GPIO26 is not pulsed as a reset. There is no STM32 reset
line on this profile.

## What v0.1.1 measures

The collector reads sensors and sends them to the app. It does not control or
diagnose the proprietary DSP inverter. No DSP protocol or wiring is included.
`dsp.present` is false. A missing DSP connector is not a fault. The original
profile still parses the existing STM32 text lines (`SYS:`, `VOUT:`, and the
rest). That parser is not started on v0.1.1, so it cannot time out or publish
zeros for a link that is not fitted. An extension point is the
`DspConnection` value in `include/board_pins.h`. It stays `NotFitted` until a
real DSP protocol is supplied.

PZEM support is the V3 Modbus-RTU map: active power and a cumulative integer
watt-hour register at 1 Wh. Address `0x01` is the manual's example; `0xF8` is
the single-slave general address and the build default. CRC is standard
Modbus, low byte first. The manual also describes the CRC bytes in the other
order; that mode is selectable and off by default. The lifetime counter is
never reset by the firmware. A failed, stale, or disconnected read is missing,
not zero. A backwards count or a jump larger than 100 A at 260 V over the
known elapsed time opens a new counter epoch and does not create negative or
huge energy. The 100 A external CT is the intended target. The seller listing
mixes 32 A and 100 A pictures, and the manufacturer page mixes V4 text. This
firmware does not claim a measured accuracy.

`inverter_energy` remains the existing BMS volt-ampere integration. It is not
measured AC watt-hours. Measured AC energy is the separate `ac_consumed`
object. Daily attribution uses Africa/Lagos, UTC+1, with no daylight-saving
shift. The DS1307 on I2C GPIO21/GPIO22 (address `0x68`, shared with the
ADS1115 at `0x48`, 100 kHz) is a ticking clock, not proof of absolute time.
A coherent read with the clock-halt bit clear stays unanchored until NTP
agrees. Saved NVS time is not promoted to trusted time, and the RTC is not a
source for command-expiry time. Setting the RTC does not reset energy ids.

Closed intervals live in the `acenergy` flash partition (192 KiB, about 1400
records, then the store stops and raises `store_full`). Unacknowledged energy
records are not deleted. The current server does not send `ac_energy_ack`, so
the device will not free those records. A gap that crosses local midnight, or
any gap without an anchored clock, stays unallocated. The unsaved window is
the open interval since the last successful flash commit, plus one torn slot.

## SD card and the ESP32 serial tee

Stated v0.1.1 wiring, not electrically verified here:

- SPI SCK GPIO18, MISO GPIO19, MOSI GPIO23
- Chip select GPIO33

GPIO33 is UART1 RX on the original profile and must not be an SD select
there. On v0.1.1 UART1 has moved to the PZEM pins, so GPIO33 is the SD select.
LoRa is not started, which is what releases the SPI pins. The original build
still starts LoRa on that bus and does not mount a card.

The v0.1.1 image links `--wrap=uart_write_bytes`. `Serial.print`,
`printf`, and `println` on UART0 still go out the console, and the same bytes
are copied into a RAM queue. A low-priority task writes that queue to
`/blackbox.bin` on the card as console records. The copy never writes the
UART, and `BBTAIL` / `BBEXPORT` set a suppress flag first, so the export text
is not stored again.

`BBTAIL` and `BBEXPORT` on the USB console, and `GET /diag/export` with the
same `API-KEY` the websocket already uses, print the recent serial records
read back from the card. Long lines are stored as `[1/n]` chunks. A line past
240 characters keeps the first chunks and ends with `truncated`. If the queue
is full, the export includes `[serial dropped N bytes]`. If the card is
absent, the reply says `sd_unavailable` and shows only the RAM copy.

Redaction applies to the stored copy only. The live console is unchanged.
`password`, `api_key`, `token`, `authorization`, `bearer`, and `secret`
values are overwritten. A line containing `OTA_CONFIG` is stored as
`OTA_CONFIG [redacted]`.

These paths are not in the tee:

- ROM boot messages before the application runs
- Panic, abort, and watchdog dumps that print through the ROM putc
- Bytes already transmitted before the application image is running
- UART1 (PZEM) and UART2 (BMS) traffic, which is not the ESP32 console

The card file is a 512 KiB preallocated log with a CRC on each 512-byte block.
FAT metadata can still tear. A torn tail is dropped on the next boot. The
unsaved window is the RAM queue plus the one block not yet flushed (up to four
records). Energy records stay on the internal `acenergy` partition, so pulling
the card does not delete them. Fault and storage lines are also mirrored to
the smaller `blackbox` flash partition. The card is not required for the
collector to keep running.

No card-detect pin, supply, pull-up, or 3.3 V versus 5 V check is claimed.
Do not treat this as wiring approval.

## Router loss and local AP

This tree did not contain `AP_FALLBACK_MIN` or a 120-second timer. The old
setup path blocked for `CONN_TIMEOUT` (10 seconds, polling every 500 ms) and
then called `WiFi.disconnect(true)`, which turns the radio off before the
provisioning AP starts. After a successful join, a later router loss never
started the AP.

Local mode is now decided in `src/wifi_fallback.cpp` and applied from
`serviceWifiFallback()`:

- No saved network: AP starts on the first service call.
- Saved network, station never confirms: AP starts 30 seconds after the
  service starts. Setup does not block on that wait.
- Station was confirmed, then stays down: AP starts 30 seconds after that
  drop. A sub-3-second up blip does not restart the 30 seconds.
- STA retries are `WiFi.begin` once, then `WiFi.reconnect` at 5-second
  intervals. Mode stays `WIFI_AP_STA`. Credentials are not cleared.
- After the station is confirmed again, the AP stays for 120 seconds, then
  stops only if nobody is joined and `/provision` has been idle. It is not
  stopped while the station is down or while onboarding is active.

The 30-second and 120-second figures are software deadlines. Association,
DHCP, and AP beacon time on a real radio are not measured here.

## Battery latch

SOC is the JK-BMS value on UART2, accepted only from a fresh dynamic frame
(15 seconds). The low threshold is 25%, not 89%. A one-time shutdown warning
is raised at 15%. At 10% the collector latches discharge inhibit until fresh
SOC stays at or above 15% for 60 seconds and every sample in that window has
JK `batStatus == 1`. SOC alone does not release the latch. A full or idle
pack (`batStatus` standby) stays latched. Recovery does not turn the output
on by itself. The new latch edge does not drive a pin
(`kPhysicalAutoOffEnabled` is false). v0.1.1 GPIO15 stays the power light.
Pre-existing `/OFF` and the OTA callback still use `applyDeviceState`.
Neither profile writes BMS FETs or a DSP command. Settings save is not
complete: see `docs/BATTERY_SETTINGS_CONTRACT.md` and
`docs/AUTH_AND_RECOVERY_EVIDENCE.md`.

## Bench still required

Host tests do not measure UART timing, SD power loss, flash wear, or watt-hour
accuracy. Before any device is updated:

- Confirm UART0 still accepts `REGISTER:` / `UNREGISTER:` and that `BBTAIL`
  prints recent `Serial` text, including a line longer than 80 characters.
- Pull the card and confirm the collector still serves BMS, PV, and commands,
  and that export says `sd_unavailable` instead of hanging.
- Run Wi-Fi, the existing OTA check, BMS, and PZEM together. PZEM must answer
  on UART1 without pausing the BMS stream for longer than one frame.
- Compare PZEM watt-hours with a reference meter on inverter and bypass paths,
  including a non-linear load. No accuracy number is claimed until that is done.
- Confirm GPIO15 can be driven after reset (it is a strapping pin) and that
  the LED supply is compatible with the ESP32.
- Confirm the DS1307 coin cell and that the shared I2C bus stays at 100 kHz.
