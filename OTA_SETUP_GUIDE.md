> **Firmware 1.0.1 reference.** The collector on this branch is firmware **2.5.0**.
> This file is kept from Dubem's tree. For the current collector (app `/v2/*`, `api.acecore.tech` vs `api-dev.acecore.tech`, factory identity), start at [README.md](README.md).

# OTA setup: a step-by-step guide

OTA means **Over-The-Air updates**: installing new firmware over Wi-Fi instead of
connecting a USB cable for every update.

You prepare each collector and each smart socket **once over USB**. After setup,
you use the admin page to send future updates.

**Start with one collector. Complete its setup before setting up the sockets.**

## 1. Understand the three different values

| Value | What it does | Where it comes from |
| --- | --- | --- |
| **MAC address / hardware ID** | Identifies the physical device. It is not a password. | The device's boot messages in Serial Monitor. Example: `AABBCCDDEE01`. |
| **OTA token** | Authorizes that particular device to download firmware updates. | Generated when you click **Create token** in the admin. It is 64 hexadecimal characters long. |
| **Cloud API key** | Lets a collector connect to the existing Acecore backend for normal operation. | Your existing `POWERCELL_API_KEY` in `include/secrets.h`, or the collector's saved settings. |

**You enter the MAC address to generate an OTA token for that hardware. The MAC
address itself is not the token.** Each collector and socket needs its own token.
Sockets do not need the collector's cloud API key for this OTA setup.

## 2. Flashing and Serial Monitor are different steps

| Action | Where you do it | What happens |
| --- | --- | --- |
| **Flash firmware** | PlatformIO's Upload command, with USB connected | Installs the program on the hardware. |
| **Send OTA settings** | Serial Monitor, after flashing | Saves the backend address, OTA token and any required credentials on the hardware. |
| **Queue an OTA update** | Firmware updates page in the admin | Makes a newer release available for the configured device to download. |

Opening Serial Monitor does **not** flash firmware. Clicking **Create token** does
**not** send that token to the hardware. You send it yourself in the configuration
step below.

## 3. Before starting

Have these ready:

- The hardware and a USB data cable.
- PlatformIO installed and this project open.
- Access to the admin's **Firmware updates** page, below **Devices** in the sidebar.
- A Wi-Fi network with Internet access.
- The HTTPS address of the backend that contains the OTA endpoints.

The local admin page is:

<http://localhost/official-powercell-admin/firmware.php>

**The local admin and the device's backend must use the same OTA database and
firmware storage.** The examples below use `https://api.acecore.tech`. Use that
address only after the OTA backend changes have been deployed there and the admin
manages that same backend. Installing the feature in local XAMPP does not deploy it
to the public server. A token created in an unrelated local database will not work
on the public server.

The hardware requires a trusted HTTPS connection for OTA. Do not use `localhost`
as the device's backend address: on the device, that means the device itself.
Server deployment details are in [OTA_UPDATES.md](OTA_UPDATES.md#backend-setup).

All terminal commands below run from the **acecore-collector project folder**.
Connect only the device you intend to flash, or select its USB port explicitly in
PlatformIO. Close Serial Monitor before uploading firmware.

## 4. Set up one collector

### Step 1 — Flash the initial firmware using USB

For an existing collector whose cloud key is in `include/secrets.h`, use:

```sh
pio run -e esp32dev_usb_bootstrap --target upload
```

This is the **USB bootstrap build**. It installs OTA support and saves the existing
cloud API key on the collector when its normal Wi-Fi connection initializes.

Use the key belonging to that collector. Do not invent a new cloud key or substitute
its MAC address or OTA token for the key.

Wait for PlatformIO to report **SUCCESS**. If this OTA-capable bootstrap build is
already installed and its cloud key is saved, skip this step.

### Step 2 — Make sure the collector has Wi-Fi

Use the collector's existing Wi-Fi setup page if its network is not already saved.
Restart it after saving the network.

The bootstrap build must successfully connect to saved Wi-Fi to initialize the
normal backend connection and import the cloud key. A successful normal connection
prints:

```text
[WS] Secure WebSocket connected
```

### Step 3 — Find this collector's MAC address

Open Serial Monitor for the connected collector:

- Baud rate: **115200**.
- Line ending: **Newline**.
- Local echo: **off** while entering credentials.

Press the hardware's reset button to see its boot messages. Find `id=` in the
`[OTA]` message, or the `[WIFI] MAC:` message. For example:

```text
[OTA] POWERCELL_FW|collector|esp32dev|1.0.0|END id=AABBCCDDEE01 provisioned=no
```

Here, **`AABBCCDDEE01` is the hardware ID**. Your hardware will show its own ID.

### Step 4 — Generate its OTA token in the admin

1. Open **Firmware updates**.
2. Find the row for this collector.
3. Expand **Set up OTA**.
4. Enter the MAC address in **Hardware MAC / ID**. Colons are accepted too.
5. Click **Create token**.
6. Copy the separate **64-character token** displayed in **Save this device credential**.

The generated token is displayed once. Keep it available until you complete the
next step. If a token already exists and you no longer have it, use **Rotate
credential → Rotate token** to generate a replacement. Rotation invalidates the old
token, so you must also send the replacement to the hardware.

### Step 5 — Send the token to the collector through Serial Monitor

Replace `PASTE_GENERATED_TOKEN_HERE` with the actual token. Send this as **one line**:

```text
OTA_CONFIG {"base_url":"https://api.acecore.tech","token":"PASTE_GENERATED_TOKEN_HERE"}
```

Click **Send** or press **Enter**. Keep the quotation marks and punctuation exactly
as shown; replace only the address if necessary and the token placeholder. Do not
paste Markdown backticks into Serial Monitor.

Expected response:

```text
[OTA] Configuration saved (credentials hidden); reboot after changing the cloud API key
```

**This is the step that saves the token on the hardware.** You do not paste it into
the firmware source code or into the MAC address field.

### Step 6 — Restart and confirm setup

Restart the collector. Keep it powered and connected to Wi-Fi.

The boot message should now show `provisioned=yes`. This means a token is saved;
it does not by itself prove the backend accepted the token.

Allow roughly **six minutes**, then refresh **Firmware updates**. The collector
should show an **installed firmware version**, a recent OTA check time, and
**Recently seen**. That version report confirms the device reached the OTA backend.

**Collector setup is complete. You do not need to flash it again for each update.**

### If you used the normal collector build instead of the bootstrap build

If its cloud API key is already saved, the token-only command above is enough.
Otherwise, send the existing cloud key with the token:

```text
OTA_CONFIG {"base_url":"https://api.acecore.tech","token":"PASTE_GENERATED_TOKEN_HERE","api_key":"PASTE_EXISTING_COLLECTOR_CLOUD_KEY_HERE"}
```

Use the existing value of `POWERCELL_API_KEY` from `include/secrets.h` for that
collector. Restart afterward. The normal release build does not import that file.

## 5. Set up one smart socket

The image in this branch is **AcePlug v0.1**, CAD-verified on Smart Socket.kicad U3: RELAY IO15, LoRa CS IO5, RST IO25, DIO0 IO26, SPI 18/19/23, LED IO4, ZMCT103 CT on ADC IO35 (EmonLib, not BL0942). The schematic symbol RFM95W-915S2 is not the air profile; the fitted module must be 433 MHz (sync 0xF1, SF9).
Do not flash it onto a v0.2 board (RELAY 27 / CS 32 / RST 26 / DIO0 34, BL0942 UART 16/17). Do not use the GPIO list in `LORA_SOCKET_PROTOCOL.md`.
QGatePass/Acecore `main` is not the merge target until KyleCI PR #1 is green for both images.
The steps below are the 1.0.1 enrollment flow.

Each socket needs its **own** MAC address and OTA token. Do not reuse the collector's
token. A socket must appear in the backend's device inventory before enrolling it.

### Step 1 — Flash the socket using USB

Connect the socket and run:

```sh
pio run -d smart-socket-firmware --target upload
```

Wait for **SUCCESS**, then open Serial Monitor at **115200 baud** with **Newline**.
Turn local echo off while entering credentials. Reset the socket and note the
hardware ID printed in its boot messages.

### Step 2 — Generate a token for this socket

In **Firmware updates**, find this socket's row. Expand **Set up OTA**, enter its
MAC address, and click **Create token**. Copy the generated 64-character token.

### Step 3 — Send its token and Wi-Fi settings

Replace all placeholders and send the following as **one line**:

```text
OTA_CONFIG {"base_url":"https://api.acecore.tech","token":"PASTE_SOCKET_TOKEN_HERE","ssid":"YOUR_WIFI_NAME","password":"YOUR_WIFI_PASSWORD"}
```

Use normal JSON escaping if a Wi-Fi name or password contains a quotation mark or
backslash: `\"` represents a quotation mark and `\\` represents a backslash.

Wait for **Configuration saved**, then restart the socket.

### Step 4 — Confirm its version appears

Keep the socket powered and within Wi-Fi range. Allow roughly **six minutes**, then
refresh the admin page. Its installed version and recent OTA check should appear.

The socket continues to use **LoRa for normal control and telemetry**. It briefly
connects to Wi-Fi to check for updates and download them. This implementation does
not send firmware through LoRa or through a collector-hosted hotspot.

Repeat these steps separately for each socket.

## 6. Send a future update — no USB setup needed

Once a device reports its installed version, use this workflow for future changes.
Do not rotate its token for every release.

### Step 1 — Give the changed firmware a newer version

For example, if the installed version is **1.0.0**, change the
`POWERCELL_FIRMWARE_VERSION` build flag to **1.0.1**:

- Collector: edit the project's root `platformio.ini`.
- Socket: edit `smart-socket-firmware/platformio.ini`.

Versions use `major.minor.patch`. The admin rejects duplicate releases and prevents
assigning the same or an older version to a device.

### Step 2 — Build the release binary

| Device | Build command | File to upload to the admin |
| --- | --- | --- |
| Collector | `pio run -e esp32dev` | `.pio/build/esp32dev/firmware.bin` |
| Smart socket | `pio run -d smart-socket-firmware` | `smart-socket-firmware/.pio/build/esp32dev/firmware.bin` |

**Do not upload the USB bootstrap binary.** It contains a collector cloud key and
is intentionally rejected by the release uploader. Normal release binaries reuse
each device's saved credentials.

### Step 3 — Upload and assign the release

1. Open **Firmware updates → Upload a release**.
2. Choose the correct `firmware.bin`, optionally add notes, and click **Upload .bin**.
3. Find the device you want to update.
4. Under **Assign release**, select the newer compatible version.
5. Click **Queue update**.
6. Watch **Update history** until the update succeeds or reports a failure.

Uploading a file only adds it to the release library. **Queue update** is the action
that assigns it to hardware. Only one update per collector and its sockets can be
active at a time.

**The target's output turns off during installation.** After successful verification,
turn it back on explicitly through the normal controls when appropriate. A failed
installation also leaves its output off.

### Step 4 — Verify the result

The device checks for updates roughly every five minutes, downloads the assigned
firmware, restarts and reports its new version after startup checks. The admin
refreshes status every 30 seconds.

Confirm both:

- Update history says **succeeded**.
- The device's **Installed** version matches the release you assigned.

A queued update or an offline device is not proof of a successful installation.

## 7. What the statuses mean

| Status or label | Meaning / next action |
| --- | --- |
| **Unknown** | No installed version has been reported to OTA yet. Complete setup or check connectivity. |
| **No release** | There is no uploaded compatible release to compare against yet. |
| **Current** | Its last reported version is at least as new as the latest compatible uploaded release. Check its last-seen time separately. |
| **Outdated** | A newer compatible release exists. Select and queue it when ready. |
| **Recently seen** | The device recently contacted the OTA endpoint over Wi-Fi. |
| **Offline / not reported** | No recent OTA check. A socket may still be communicating over LoRa. |
| **queued** | Waiting for the device to collect its assignment. It can still be cancelled. |
| **downloading** | The assignment has been delivered and download is expected or underway. This is not a percentage-complete measurement. |
| **rebooting** | The device reported that it is restarting into the new firmware. Wait for its version report. |
| **succeeded** | The device reported the assigned version after healthy startup. |
| **failed** | The update failed or rolled back. Read the error, resolve it, and explicitly queue another attempt. |
| **cancelled** | The queued assignment was cancelled before delivery. |

## 8. Common questions and problems

**Do I create a token or enter a MAC address?**
Enter the MAC address in the setup form. The admin creates a separate token for it.
Copy the generated token into the serial `OTA_CONFIG` command.

**Where do I paste the OTA_CONFIG line?**
In Serial Monitor's input for the hardware connected by USB. Not in a terminal
shell, the source code, or the admin MAC field.

**Do I need a new token for every update?**
No. Saved settings survive normal restarts and firmware updates. Reconfigure after
erasing device storage or rotating credentials. For a socket Wi-Fi change, send
the full socket command again with its token and new Wi-Fi details.

**I closed the token display without saving it.**
Rotate the token for that same hardware row and send the replacement to the device.
Do not rotate a credential while an update is active.

**Serial Monitor shows nothing after I send the command.**
Check the selected USB port, 115200 baud and Newline. Confirm the hardware actually
has OTA-capable firmware. Keep the command on one line.

**I see “Invalid configuration JSON” or “Invalid token”.**
Check quotation marks and commas. The token must be the generated 64-character
hexadecimal string, with no added spaces. The example placeholder is not a token.

**I see “Busy; retry configuration later”.**
Wait for the current OTA check to finish, then resend the complete command.

**The device says provisioned=yes but the admin still says Unknown.**
Verify its MAC matches the enrolled row, its token is the latest token for that row,
and Wi-Fi has Internet access. Check that the configured HTTPS backend contains
the same enrollment and releases as the admin you are viewing. The backend OTA
routes and trusted HTTPS certificate must be deployed. Wait roughly six minutes
for a check; an unreachable server will not receive a version report.

**The collector reports “API key missing”.**
Its normal cloud key is not saved. Use the bootstrap build with the correct existing
`POWERCELL_API_KEY` and a working saved Wi-Fi connection, or send the full collector
configuration containing `api_key`, then restart.

**I uploaded a .bin but nothing updated.**
Uploading does not assign it. Select the release for a configured device and click
**Queue update**. The selected version must be newer than the installed version.

**My socket works over LoRa but cannot update.**
OTA needs the Wi-Fi settings supplied in its configuration, Wi-Fi coverage and
Internet access. LoRa reception alone is not enough for OTA downloads.

## 9. Completion checklist

For each physical device:

- [ ] OTA-capable firmware has been flashed over USB.
- [ ] Its own MAC address is registered in the correct admin row.
- [ ] Its own generated token has been sent through Serial Monitor.
- [ ] The device has the required Wi-Fi connection and correct HTTPS backend address.
- [ ] The admin shows its installed firmware version and a recent OTA check.

Once all five are complete, future updates use **build → upload .bin → queue →
verify success**. You do not repeat USB setup for each release.

For deployment, protocol details and hardware recovery testing, see
[OTA_UPDATES.md](OTA_UPDATES.md). Physical update and power-loss testing is still
required before a production rollout.
