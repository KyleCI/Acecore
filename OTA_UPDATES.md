# Collector and smart-socket OTA

For the operator walkthrough, start with [OTA_SETUP_GUIDE.md](OTA_SETUP_GUIDE.md).
This document covers deployment and technical behavior.

The admin is `/Applications/XAMPP/htdocs/official-powercell-admin/firmware.php`.
The API is `/Applications/XAMPP/htdocs/official-powercell-backend`.
The `htdocs` directory is a symlink to `xamppfiles/htdocs` on this machine.

## What is implemented

- Separate collector/socket releases. Version, ESP32 model and type come from an
  identity embedded in `firmware.bin`, not an editable upload form.
- Admin upload, release notes, full existing collector/socket inventory, per-device
  enrollment, version comparison, update assignment, queued cancellation and history.
- A unique revocable OTA token per physical device, authenticated HTTPS downloads,
  SHA-256 and binary identity verification, and inactive-slot installation.
- Devices check roughly every five minutes (with jitter). Admin refreshes reported
  status every 30 seconds. Offline devices retain their last reported version.
- One active update per collector and its sockets. Uploading a release does not
  schedule any updates. Devices without OTA setup/version reports show Unknown.
- Failed downloads leave the current application intact. New applications must
  complete 30 seconds of main-loop health checks before accepting the OTA image.
  The pinned ESP32 framework enables bootloader rollback; a reset before validation
  returns to the previous application. Hardware failure detected in the main loop
  also requests rollback after 90 seconds. A hung application still needs a watchdog
  or reset to trigger bootloader recovery; the health gate does not prove every
  sensor or backend integration is correct.

## Socket connectivity

LoRa remains the socket control/telemetry transport. Sockets temporarily join a
**configured Wi-Fi network with Internet access** for OTA; the collector does not
relay firmware binaries or provide an OTA hotspot in this implementation. OTA runs
in a background task so current sampling, overcurrent protection and LoRa processing
continue. Each socket reports its installed version in LoRa telemetry as `FW:x.y.z`
and directly to the authenticated OTA endpoint. The collector includes received
socket versions and their telemetry age in its own telemetry.

A socket outside Wi-Fi range continues its normal LoRa operation, but cannot fetch
an update until Wi-Fi is available. Firmware download over LoRa and a collector-hosted
Wi-Fi fallback are not implemented.

## Backend setup

1. Apply `database/migrations/014_firmware_ota.sql` to the same database used by the
   backend and admin. This only creates three new OTA tables.
2. Give the PHP service account read/write access to the private firmware directory.
   Default: `<backend>/storage/firmware`. Both admin and API must use the same path.
   If configured, set `FIRMWARE_STORAGE_PATH` for both processes. The admin reads its
   environment directly; it does not load the backend's `.env`.
3. If the projects are not sibling folders, set `POWERCELL_BACKEND_PATH` in the admin
   environment. Keep firmware storage inaccessible to direct HTTP requests. A
   `storage/.htaccess` denies access for Apache deployments that honor overrides.
4. Serve the API through HTTPS and forward Authorization headers. The firmware
   uses the existing ISRG Root X1 trust anchor in `include/websocket_ca.h`; the
   deployment's certificate chain must validate against it. HTTP/redirect downloads
   and arbitrary manifest-provided hosts are rejected. Local HTTP-only XAMPP can
   exercise the admin, but hardware downloads require a trusted HTTPS endpoint.
5. PHP upload and POST limits must exceed 1.25 MiB (for example 2M and 3M). Firmware
   is limited to the existing 0x140000-byte OTA slot. SPIFFS assets are not updated.

## Initial USB setup

Every existing collector/socket needs one OTA-capable USB flash first. Do not erase
NVS; it holds network settings and credentials. The default partition table remains
unchanged. Confirm actual hardware uses that table before the first rollout.

In the admin, expand **Set up OTA** for an existing device and enter the hardware ID
printed at boot (12 hexadecimal MAC digits; colons are accepted by the form). Copy
its one-time token. The socket must already be registered in the backend inventory.

Build/flash a collector with an existing local API key:

```sh
pio run -e esp32dev_usb_bootstrap --target upload
```

This USB-only build imports `include/secrets.h`'s `POWERCELL_API_KEY` into NVS. The
admin rejects this binary because it contains a device credential. Normal release
builds exclude `secrets.h` and reuse the API key saved on each collector.

Alternatively flash a normal collector build and supply `api_key` in the serial
configuration below. Configure collector Wi-Fi through its existing provisioning UI.

```sh
pio run -e esp32dev --target upload
pio run -d smart-socket-firmware --target upload
```

Send one line in the corresponding USB serial monitor at 115200 baud:

```text
OTA_CONFIG {"base_url":"https://api.acecore.tech","token":"64_HEXADECIMAL_CHARACTERS_FROM_ADMIN","api_key":"EXISTING_COLLECTOR_CLOUD_KEY"}
```

`api_key` is optional if a collector already has it saved. Reboot after changing it.
For each socket use its own token and network credentials:

```text
OTA_CONFIG {"base_url":"https://api.acecore.tech","token":"64_HEXADECIMAL_CHARACTERS_FROM_ADMIN","ssid":"YOUR_WIFI","password":"YOUR_WIFI_PASSWORD"}
```

Use the actual backend origin/base path for `base_url`. Credentials are saved in
NVS and are not echoed by this handler. Turn off serial-monitor local echo while
entering them. Wi-Fi reconnects and OTA checks never send credentials over LoRa.
NVS is persistent storage; this change does not enable flash/NVS encryption.

## Publishing subsequent releases

1. Increase `POWERCELL_FIRMWARE_VERSION` in the relevant `platformio.ini`, using
   `major.minor.patch`, such as `1.0.1`.
2. Build only the intended release:

   ```sh
   pio run -e esp32dev
   pio run -d smart-socket-firmware
   ```

3. Upload `.pio/build/esp32dev/firmware.bin` for collectors, or
   `smart-socket-firmware/.pio/build/esp32dev/firmware.bin` for sockets.
4. Wait for a healthy version report, then select a newer compatible release for
   one target and click **Queue update**. Assign the next device after completion.
5. Success requires the device to report the new version after startup validation.
   A disconnected device is not assumed to have updated. Failed jobs require a new
   explicit assignment; failures do not trigger endless automatic retry loops.

The target's output turns **off before installation**. Collectors boot off already;
updated sockets persist an OTA hold-off flag and stay off until an explicit ON
command clears it. Failed installations also leave the output off. Normal socket
power-up behavior outside an OTA hold-off is preserved. Plan maintenance around
this interruption. Current sensing and overcurrent protection remain active.

## Protocol

- `POST /api/firmware/check`: Bearer device OTA token; JSON `device_id` (MAC without
  colons), `kind`, `model`, `version`, `healthy`, `deployment_id`, `status`, `error`.
- Response: `{"update":null}` or an update containing deployment ID, kind, model,
  version, SHA-256, size and a same-origin `/api/firmware/download/{id}` path.
- `GET /api/firmware/download/{id}`: same token, authorized only for that device's
  active download. No public binary URLs and no credentials in URLs.
- States: queued → downloading → rebooting → succeeded/failed. Queued jobs can be
  cancelled before their manifests are delivered. Offline active jobs remain
  visible; reconnect/recovery is needed before updating another device in that group.

## Verification and hardware acceptance

```sh
php tests/firmware_ota_test.php       # from backend directory
php tests/firmware_upload_test.php   # isolated local HTTP upload test
```

Before production rollout, perform a bench update from 1.0.0 to 1.0.1 on one collector
and one socket. Verify telemetry/control during Wi-Fi activity, safe relay output,
credential preservation, corrupt/download-truncated rejection, lost network,
interrupted power and rollback on a bad new application's startup. Compilation and
server tests do not replace physical flash/power-loss testing.
