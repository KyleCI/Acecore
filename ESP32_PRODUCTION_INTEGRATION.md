# Acecore PowerCell ESP32 WebSocket Protocol

This document describes the WebSocket behavior verified by the local Node.js
simulator. Use it as the reference when implementing or repairing the
production ESP32 firmware.

## 1. System overview

The ESP32 maintains one persistent WebSocket connection to the PowerCell
backend. The backend sends switching commands over that connection. The ESP32
operates the requested relay, returns a command result, and publishes its latest
state.

```text
Application / HTTP API
        |
        | set_state request
        v
PowerCell backend / WebSocket server
        |
        | persistent WebSocket connection
        v
ESP32 gateway -> relay hardware
        |
        +-> command_result
        +-> state_snapshot
```

The ESP32 is the WebSocket client. It must not open a new connection for every
command. The backend stores the active connection by `device_id` and uses that
connection to deliver commands.

## 2. Connection configuration

### Local development

```text
ws://127.0.0.1:8081/ws/device?device_id=C8%3A85%3A41%3A2C%3A31%3AD4
```

### Production

```text
wss://api.acecore.tech/ws/device?device_id=<URL_ENCODED_DEVICE_ID>
```

Production must use `wss://` and validate the server certificate against a
trusted CA certificate. Do not disable TLS verification in production.

### WebSocket handshake

The request must contain:

```http
GET /ws/device?device_id=C8%3A85%3A41%3A2C%3A31%3AD4 HTTP/1.1
Host: api.acecore.tech
Upgrade: websocket
Connection: Upgrade
API-KEY: <DEVICE_API_KEY>
```

Requirements:

- `device_id` is required as a URL query parameter.
- `API-KEY` is required as an HTTP upgrade header.
- The device ID must uniquely identify the physical gateway.
- Credentials must be provisioned securely and must not be committed to source
  control.

The server rejects an invalid path, missing device ID, or invalid API key before
upgrading the connection.

## 3. Connection lifecycle

The ESP32 should implement the following lifecycle:

1. Initialize networking, GPIO pins, relays, and stored state.
2. Synchronize the clock using NTP. Correct time is required for command expiry
   validation and TLS certificate validation.
3. Connect to Wi-Fi or the configured network.
4. Open the authenticated WebSocket connection.
5. After connection, publish an initial `state_snapshot`.
6. Listen continuously for WebSocket messages.
7. Respond to valid `set_state` commands.
8. Publish a periodic snapshot, currently every 30 seconds.
9. Maintain the connection using WebSocket ping/pong handling.
10. If disconnected, reconnect after a delay. The local reference uses five
    seconds.
11. After reconnecting, publish a new snapshot so the backend can reconcile
    actual device state.

Only one active connection should exist for a device ID. A second connection
using the same ID may replace the first connection on the backend.

## 4. Time format

All protocol timestamps are integer Unix timestamps in seconds, in UTC.

Example:

```json
{
  "timestamp": 1786572360
}
```

Do not send milliseconds. On ESP32 platforms, convert millisecond clocks where
necessary and use synchronized wall-clock time for `expires_at` checks.

## 5. Server-to-device messages

### 5.1 Connection acknowledgement

The server may send this immediately after the WebSocket handshake:

```json
{
  "type": "connection_ack",
  "status": "connected",
  "device_id": "C8:85:41:2C:31:D4",
  "server_time": 1786572110
}
```

The ESP32 may log this message. It should not treat it as an error. The
`server_time` value may be used for diagnostics, but should not replace proper
NTP clock synchronization without an explicitly designed clock policy.

### 5.2 Set-state command

Socket command:

```json
{
  "type": "set_state",
  "command_id": "cmd-test-001",
  "target_type": "socket",
  "target_id": 3,
  "desired_state": "on",
  "expires_at": 1786572660
}
```

Gateway command:

```json
{
  "type": "set_state",
  "command_id": "cmd-test-002",
  "target_type": "gateway",
  "target_id": "ACE2025",
  "desired_state": "off",
  "expires_at": 1786572660
}
```

Field definitions:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Must be `set_state`. |
| `command_id` | string | Unique command identifier, 1 to 47 characters. |
| `target_type` | string | Either `socket` or `gateway`. |
| `target_id` | integer/string | Socket number or gateway identifier. |
| `desired_state` | string | Either `on` or `off`. |
| `expires_at` | integer | Unix timestamp after which the command must be rejected. |

## 6. Device-to-server messages

### 6.1 State snapshot

Send a snapshot immediately after connecting, periodically, and after a
confirmed command:

```json
{
  "type": "state_snapshot",
  "gateway_id": "C8:85:41:2C:31:D4",
  "gateway_name": "ACE2025",
  "online": true,
  "timestamp": 1786572360,
  "sockets": [
    { "socket_id": 0, "state": "off" },
    { "socket_id": 1, "state": "off" },
    { "socket_id": 2, "state": "off" },
    { "socket_id": 3, "state": "on" }
  ]
}
```

The current simulator uses zero-based socket IDs. Confirm that the production
database, UI, and physical relay mapping also use zero-based IDs before deploying
firmware. If production uses one-based IDs, define and consistently apply the
translation at a single system boundary.

The existing reference payload places the device ID in `gateway_id` and the
human/application gateway identifier in `gateway_name`. Confirm these field
semantics with the production backend before freezing the firmware contract.

### 6.2 Confirmed command result

After successfully applying and verifying a command:

```json
{
  "type": "command_result",
  "gateway_id": "C8:85:41:2C:31:D4",
  "command_id": "cmd-test-001",
  "target_type": "socket",
  "target_id": 3,
  "desired_state": "on",
  "result": "confirmed",
  "timestamp": 1786572360
}
```

### 6.3 Rejected command result

If validation or hardware operation fails:

```json
{
  "type": "command_result",
  "gateway_id": "C8:85:41:2C:31:D4",
  "command_id": "cmd-test-003",
  "target_type": "socket",
  "target_id": 999,
  "desired_state": "on",
  "result": "rejected",
  "timestamp": 1786572360,
  "error": "Socket 999 does not exist. Valid range: 0-7."
}
```

Never report `confirmed` merely because a JSON message was accepted. Report it
only after the requested GPIO/relay operation has completed. If the hardware
provides relay-state feedback, use that feedback for confirmation. Otherwise,
document that confirmation means the output operation completed without a
detected software error.

## 7. Command validation

Validate every command before changing GPIO state. Apply the checks in this
order:

1. The payload is a valid JSON object.
2. `type` equals `set_state`.
3. `command_id` is a non-empty string no longer than 47 characters.
4. `target_type` is `socket` or `gateway`.
5. `expires_at` is an integer Unix timestamp and is greater than the current
   timestamp.
6. `desired_state` is `on` or `off`.
7. For a gateway command, `target_id` matches the configured gateway ID or
   device ID.
8. For a socket command, `target_id` is an integer within the configured socket
   range.
9. The requested relay operation succeeds.

If any check fails, do not change relay state. Return a rejected
`command_result` containing a useful `error` value.

## 8. Command execution sequence

For a valid socket command:

```text
receive JSON
  -> parse
  -> validate
  -> map socket_id to GPIO
  -> set relay output
  -> verify/store resulting state
  -> send command_result
  -> send state_snapshot
```

For a valid gateway command, apply the requested state to every configured
socket. Define how partial hardware failure is handled. The safest default is to
return `rejected` with an error if all requested outputs cannot be confirmed,
then publish a snapshot containing the actual state of every socket.

## 9. Duplicate command protection

Production firmware should remember recently processed `command_id` values.
WebSocket reconnects and backend retries can deliver the same command more than
once.

Recommended behavior:

- Keep a bounded cache of recent command IDs and their results.
- If a duplicate arrives, do not operate the relay a second time.
- Resend the stored `command_result` and current `state_snapshot`.
- Persist the cache only if commands must remain idempotent across ESP32
  reboots; otherwise define reboot behavior explicitly.

Even though setting a relay to a state is naturally more idempotent than a
toggle operation, duplicate detection still prevents unnecessary hardware
activity and produces reliable audit results.

## 10. Relay and state handling

Create one explicit mapping between protocol socket IDs and GPIO pins:

```text
socket_id 0 -> GPIO <board-specific pin>
socket_id 1 -> GPIO <board-specific pin>
...
```

Account for active-low relay boards. In that case, logical `on` may require a
LOW GPIO value and logical `off` may require HIGH.

At boot:

- Configure safe GPIO levels before enabling relay outputs.
- Avoid transient relay activation during initialization.
- Decide whether the device restores saved states or initializes all sockets
  to `off`.
- Publish the actual initialized state after connecting.

State snapshots must represent actual current state, not only the most recent
requested state.

## 11. WebSocket health and reconnection

The connection must remain responsive while relay commands are processed.
Avoid long blocking delays in the WebSocket event loop.

Recommended reconnection behavior:

- Detect socket close, network loss, authentication failure, and TLS failure.
- Reconnect automatically with backoff and a maximum delay.
- Add small random jitter when many devices may reconnect simultaneously.
- Do not create overlapping connection attempts.
- Republish state immediately after reconnection.
- Log close/error reasons without logging the API key.

The WebSocket library may handle protocol ping frames automatically. If the
backend sends an application JSON message with `type: "ping"`, reply with:

```json
{
  "type": "pong",
  "gateway_id": "C8:85:41:2C:31:D4",
  "timestamp": 1786572360
}
```

## 12. Security requirements

- Use `wss://` in production.
- Validate the TLS certificate and hostname.
- Never compile a shared production API key directly into a public repository.
- Prefer unique credentials per physical device.
- Support credential rotation or secure reprovisioning.
- Never print the API key in serial logs.
- Reject commands for the wrong device or gateway.
- Validate JSON types and lengths before using values.
- Bound incoming message sizes to protect ESP32 memory.
- Do not dynamically allocate large unbounded JSON documents.
- Keep firmware, trusted CA certificates, and cryptographic libraries updated.

The API key used during local testing has appeared in terminal output. If it is
a production credential, rotate it before deployment.

## 13. Suggested firmware structure

Keep networking, protocol validation, and hardware control separate:

```text
config/
  device_config        device ID, gateway ID, URL, non-secret defaults
network/
  wifi_manager         network connection and recovery
  websocket_client     authenticated connection and reconnect behavior
protocol/
  message_parser       JSON parsing and message dispatch
  command_validator    field and expiry validation
  message_builder      command results and snapshots
hardware/
  relay_controller     socket-to-GPIO mapping and physical operations
state/
  device_state         current socket states and recent command IDs
```

Exact filenames depend on whether the project uses Arduino, PlatformIO, or
ESP-IDF. The separation is more important than the names.

## 14. Local reference test

Run each service in a separate terminal:

```bash
npm run server
npm start
npm run api
```

Send a test command:

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -H "API-KEY: YOUR_API_KEY" \
  -d '{
    "command_id": "cmd-test-001",
    "target_type": "socket",
    "target_id": 3,
    "desired_state": "on",
    "expires_at": 9999999999
  }' \
  "http://127.0.0.1:8082/api/turn-on-device?device_id=C8%3A85%3A41%3A2C%3A31%3AD4"
```

Expected sequence:

1. The HTTP API returns `202` with `delivery_status: "sent"`.
2. The WebSocket server sends `set_state` to the connected device.
3. The simulator logs the relay operation.
4. The simulator sends `command_result` with `result: "confirmed"`.
5. The next snapshot reports socket 3 as `on`.

The HTTP `202` response means the command was delivered to the WebSocket
connection. It is not hardware confirmation. The authoritative completion
message is the ESP32's `command_result`.

## 15. Production acceptance checklist

Before deploying the firmware, verify all of the following:

- [ ] Production `wss://` connection succeeds with certificate validation.
- [ ] Invalid API credentials are rejected.
- [ ] Initial state is published after every connection.
- [ ] Periodic snapshots contain all physical sockets.
- [ ] Every socket ID controls the correct relay.
- [ ] Active-low/active-high behavior is correct.
- [ ] Valid `on` and `off` socket commands are confirmed.
- [ ] Gateway commands operate all intended sockets.
- [ ] Expired commands are rejected without hardware changes.
- [ ] Unknown socket and gateway targets are rejected.
- [ ] Invalid JSON does not crash or reset the ESP32.
- [ ] Duplicate command IDs do not cause duplicate operations.
- [ ] A state snapshot follows every accepted command.
- [ ] Wi-Fi loss triggers safe reconnection.
- [ ] Backend restart triggers safe reconnection.
- [ ] ESP32 reboot restores the documented safe state.
- [ ] API keys never appear in logs or source control.
- [ ] Long-duration testing shows no memory growth or watchdog resets.

## 16. Items to confirm against production

The following details are represented by the simulator but should be confirmed
before treating them as a permanent production contract:

1. Whether socket IDs are zero-based (`0-7`) or one-based (`1-8`).
2. Whether `gateway_id` should contain the device ID or `ACE2025`-style gateway
   identifier.
3. Whether `gateway_name` is the correct field for the application gateway ID.
4. The maximum allowed `command_id` length.
5. Required behavior when only some relays in a gateway command succeed.
6. How long duplicate command results must be retained.
7. Whether the backend uses protocol ping frames, JSON `ping` messages, or both.
8. The production CA certificate/provisioning and credential-rotation process.

Resolve these items with the production backend schema and firmware source
before release.
