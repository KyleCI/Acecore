# Acecore PowerCell ESP32 Firmware

Production ESP32 collector firmware for the Acecore PowerCell platform. The
collector maintains one authenticated TLS WebSocket connection to
`wss://api.acecore.tech/ws/device`, controls its local output and ESP-NOW smart
sockets, and publishes confirmed state.

The complete wire protocol and production acceptance checklist are in
[`ESP32_PRODUCTION_INTEGRATION.md`](ESP32_PRODUCTION_INTEGRATION.md).

## OTA updates

Start with [OTA_SETUP_GUIDE.md](OTA_SETUP_GUIDE.md) for step-by-step USB setup,
MAC addresses, tokens, cloud keys, and future updates.
See [OTA_UPDATES.md](OTA_UPDATES.md) for deployment and technical details.

## Configure credentials

Normal builds read the gateway API key from NVS. Provision it over USB using
`OTA_CONFIG` as described in the OTA guide. An existing `include/secrets.h` can
be imported with the USB-only `esp32dev_usb_bootstrap` environment. The file is
ignored by git and excluded from normal release binaries. Wi-Fi is normally configured through the device provisioning
page and stored in NVS; optional fallback definitions also exist in the secrets
file.

If the credential previously committed in `include/config.h` was valid, rotate
it before deploying this firmware.

## Build

Install PlatformIO, then run:

```bash
pio run
```

Upload firmware and the SPIFFS web assets:

```bash
pio run --target upload
pio run --target uploadfs
pio device monitor
```

The serial monitor runs at 115200 baud.

## Runtime behavior

- Synchronizes UTC time with NTP before accepting expiring commands.
- Connects with the URL-encoded Wi-Fi MAC as `device_id` and sends the API key
  only in the WebSocket upgrade header.
- Validates TLS with the CA in `include/websocket_ca.h`.
- Publishes a state snapshot on connection, every 30 seconds, and after commands.
- Confirms socket commands only after the socket's ESP-NOW acknowledgement.
- Applies gateway commands to the collector output and all configured sockets.
- Replays cached command results for duplicate command IDs without switching
  hardware again.
- Reconnects automatically and uses WebSocket heartbeat ping/pong handling.

## Production checks

Before flashing production devices, verify socket ID mapping, active-high versus
active-low relay behavior, the `gateway_id`/`gateway_name` backend semantics, and
the current CA chain. Exercise the accepted and rejected command cases listed in
the integration document and run a long-duration reconnect test.
