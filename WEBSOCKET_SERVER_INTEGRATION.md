# PowerCell WebSocket Server Integration Contract

This document defines the server-side contract for communicating with the PowerCell ESP32 collector firmware.

## 1. Purpose

The ESP32 opens a persistent secure WebSocket connection to the backend. The server uses that connection to request ON/OFF state changes for:

- The main PowerCell gateway relay.
- Smart sockets managed by the gateway over ESP-NOW.

The ESP32 reports command progress, confirmed hardware state, faults, and complete state snapshots back to the server.

The ESP32 uses this same persistent WebSocket for commands, immediate state synchronization, and periodic telemetry. No second HTTPS connection is opened.

## 2. Connection endpoint

Default firmware configuration:

```text
wss://api.acecore.tech/ws/device?device_id=<ESP32_MAC_ADDRESS>
```

Example:

```text
wss://api.acecore.tech/ws/device?device_id=AA:BB:CC:DD:EE:FF
```

Configuration is located in `include/config.h`:

```cpp
const char* const webSocketHost = "api.acecore.tech";
const uint16_t    webSocketPort = 443;
const char* const webSocketPath = "/ws/device";
```

The server must support a normal RFC 6455 WebSocket connection over TLS.

## 3. Authentication

The ESP32 sends the existing API key during the WebSocket upgrade request:

```http
API-KEY: <configured API key>
```

The server should reject the upgrade when:

- The API key is missing.
- The API key is invalid or revoked.
- The `device_id` is missing or not authorized for that credential.

The server should associate the accepted connection with the `device_id` query parameter. In production, use a unique credential for each gateway instead of one shared global API key.

## 4. Connection behavior

The ESP32:

- Reconnects approximately every five seconds after disconnection.
- Uses WebSocket ping/pong heartbeat handling.
- Sends a `state_snapshot` after connecting.
- Sends a `telemetry` message every five seconds while connected.
- Continues local sensing and safety behavior while offline.
- Uses NTP to validate command expiry timestamps.

The server should maintain a connection registry indexed by gateway MAC address. A newly connected session should replace any older session registered for the same gateway.

## 5. Server-to-device command

The firmware currently accepts one command type: `set_state`.

### 5.1 Gateway relay command

```json
{
  "type": "set_state",
  "command_id": "cmd-01JABC123",
  "target_type": "gateway",
  "target_id": "ACE1100",
  "desired_state": "on",
  "expires_at": 1785840030
}
```

### 5.2 Smart-socket command

```json
{
  "type": "set_state",
  "command_id": "cmd-01JABC124",
  "target_type": "socket",
  "target_id": 3,
  "desired_state": "off",
  "expires_at": 1785840030
}
```

### 5.3 Command fields

| Field | Type | Required | Description |
|---|---:|---:|---|
| `type` | string | Yes | Must be exactly `set_state`. |
| `command_id` | string | Yes | Unique idempotency ID, maximum 47 characters. |
| `target_type` | string | Yes | `gateway` or `socket`. |
| `target_id` | string or integer | Yes | Gateway name or smart-socket device ID. |
| `desired_state` | string | Yes | Must be exactly `on` or `off`, lowercase. |
| `expires_at` | integer | Recommended | Unix timestamp in UTC seconds. `0` or omission disables expiry checking. |

Unknown message types are ignored.

### 5.4 Command ID requirements

Every command must have a unique `command_id`.

The ESP32 keeps a small in-memory cache of recently processed IDs. If the server resends a recently handled command, the device returns a `duplicate` result instead of operating the relay again.

Command IDs should therefore:

- Be globally unique, such as a UUID or ULID.
- Remain the same when retrying the same logical command.
- Be changed when the user creates a genuinely new command.
- Fit within 47 ASCII characters.

## 6. Device-to-server command results

The ESP32 sends `command_result` messages as command processing advances.

### 6.1 Gateway confirmation

The gateway relay is operated locally and persisted immediately:

```json
{
  "type": "command_result",
  "command_id": "cmd-01JABC123",
  "gateway_id": "AA:BB:CC:DD:EE:FF",
  "target_id": "ACE2025",
  "result": "confirmed",
  "reported_state": "on",
  "timestamp_ms": 245901
}
```

### 6.2 Smart-socket acceptance

The gateway first forwards the command to the socket over ESP-NOW:

```json
{
  "type": "command_result",
  "command_id": "cmd-01JABC124",
  "gateway_id": "AA:BB:CC:DD:EE:FF",
  "target_id": "3",
  "result": "accepted",
  "reported_state": "switching",
  "timestamp_ms": 246100
}
```

`accepted` does not mean the relay has switched. The server should show a pending or switching state and wait for the final result.

### 6.3 Smart-socket confirmation

After receiving the socket's ESP-NOW acknowledgement:

```json
{
  "type": "command_result",
  "command_id": "cmd-01JABC124",
  "gateway_id": "AA:BB:CC:DD:EE:FF",
  "target_id": "3",
  "result": "confirmed",
  "reported_state": "off",
  "timestamp_ms": 246350
}
```

### 6.4 Command timeout

If the smart socket does not acknowledge within approximately five seconds:

```json
{
  "type": "command_result",
  "command_id": "cmd-01JABC124",
  "gateway_id": "AA:BB:CC:DD:EE:FF",
  "target_id": "3",
  "result": "failed",
  "reported_state": "on",
  "reason": "hardware_timeout",
  "timestamp_ms": 251100
}
```

### 6.5 Rejected command

```json
{
  "type": "command_result",
  "command_id": "cmd-01JABC125",
  "gateway_id": "AA:BB:CC:DD:EE:FF",
  "target_id": "3",
  "result": "rejected",
  "reported_state": "off",
  "reason": "overcurrent_lockout",
  "timestamp_ms": 252000
}
```

### 6.6 Result values

| Result | Final? | Meaning |
|---|---:|---|
| `accepted` | No | Command was accepted and forwarded; hardware confirmation is pending. |
| `confirmed` | Yes | The requested state was applied and confirmed. |
| `rejected` | Yes | The command was not allowed or was invalid. |
| `failed` | Yes | The command began but hardware confirmation failed. |
| `duplicate` | Yes | This command ID was recently processed and was not executed again. |

### 6.7 Failure reasons

The firmware can currently return:

| Reason | Meaning |
|---|---|
| `invalid_command` | Required fields or state values are invalid. |
| `invalid_target_type` | `target_type` is neither `gateway` nor `socket`. |
| `unknown_target` | The requested smart-socket ID is not registered on the gateway. |
| `overcurrent_lockout` | Local safety protection prevents switching the socket. |
| `command_expired` | `expires_at` is in the past and device time is synchronized. |
| `command_queue_full` | The ESP32 command queue is temporarily full. |
| `hardware_timeout` | No ESP-NOW confirmation arrived within the timeout. |

`timestamp_ms` is ESP32 uptime in milliseconds, not Unix time. Use it for diagnostics and ordering within one boot, not as a database wall-clock timestamp. The server should record its own receive time.

## 7. State snapshots

The ESP32 sends a complete `state_snapshot`:

- Immediately after the WebSocket connects.
- After a gateway state change.
- When a smart-socket command starts.
- When a smart-socket confirms or times out.

Example:

```json
{
  "type": "state_snapshot",
  "gateway_id": "F4:2D:C9:C0:90:A8",
  "device_name": "ACE1100",
  "gateway": {
    "reported_state": "on",
    "status": "on"
  },
  "sockets": [
    {
      "id": 3,
      "reported_state": "off",
      "status": "off",
      "overcurrent": false
    },
    {
      "id": 4,
      "reported_state": "off",
      "status": "fault",
      "overcurrent": true
    }
  ]
}
```

### 7.1 Snapshot fields

| Field | Type | Description |
|---|---:|---|
| `gateway_id` | string | ESP32 station MAC address. This is the primary connection identity. |
| `device_name` | string | Configured logical gateway name, currently `ACE2025`. |
| `gateway.reported_state` | string | Confirmed main relay state: `on` or `off`. |
| `gateway.status` | string | Current gateway status, presently `on` or `off`. |
| `sockets` | array | All smart sockets currently approved in gateway memory. |
| `sockets[].id` | integer | Smart-socket device ID used as `target_id` in commands. |
| `sockets[].reported_state` | string | Last known relay state: `on` or `off`. |
| `sockets[].status` | string | `on`, `off`, `switching`, or `fault`. |
| `sockets[].overcurrent` | boolean | Whether local overcurrent protection is active. |

## 8. Desired state versus reported state

The backend should store these separately:

- `desired_state`: the state requested by the user.
- `reported_state`: the last state confirmed or reported by the ESP32.
- `status`: operational condition such as `switching`, `fault`, or `offline`.

Recommended transition:

```text
User requests ON
    -> desired_state = on
    -> status = switching
    -> send set_state command
    -> receive accepted (socket only)
    -> keep status = switching
    -> receive confirmed
    -> reported_state = on
    -> status = on
```

On rejection or failure:

```text
desired_state remains recorded as the user's request
reported_state remains the last confirmed hardware state
status becomes fault or command_failed
failure reason is stored with the command
```

Do not set `reported_state` merely because the server successfully wrote a WebSocket frame.

## 9. Meaning of idle and offline

The firmware does not accept `idle` as a requested relay state. Valid commands are only `on` and `off`.

The server may derive additional statuses:

- `idle`: relay is ON but telemetry current/power remains below a configured threshold.
- `offline`: no active WebSocket connection or no heartbeat within the server timeout.
- `fault`: snapshot reports overcurrent or a command is rejected for a protection reason.
- `switching`: command accepted but not yet confirmed.

Do not send `desired_state: "idle"`; the firmware will reject it as an invalid command.

## 10. Recommended server data model

### Gateways

```text
id
gateway_mac                 unique
device_name
desired_state               on/off
reported_state              on/off/unknown
status                      on/off/switching/fault/offline
last_seen_at
connected_at
disconnected_at
```

### Smart sockets

```text
id
gateway_id                  foreign key
socket_device_id            integer
desired_state               on/off
reported_state              on/off/unknown
status                      on/off/switching/fault/offline
overcurrent                 boolean
last_seen_at
unique(gateway_id, socket_device_id)
```

### Device commands

```text
id / command_id             UUID or ULID, unique
gateway_id
target_type                 gateway/socket
target_id
desired_state               on/off
result                      queued/accepted/confirmed/rejected/failed/duplicate
failure_reason              nullable
created_at
sent_at
accepted_at                 nullable
completed_at                nullable
expires_at
```

## 11. Recommended server workflow

When a user requests a state change:

1. Authenticate and authorize the user.
2. Verify that the target belongs to the selected gateway.
3. Generate a unique command ID.
4. Store the command with `result = queued` and an expiry 20–30 seconds ahead.
5. Update `desired_state` but not `reported_state`.
6. Check whether the gateway has an active WebSocket.
7. If connected, serialize and send the `set_state` command.
8. If disconnected, mark the command as unsent/offline or retain it only until expiry according to product policy.
9. Process `command_result` messages and update the command record.
10. Process `state_snapshot` messages as the authoritative last reported hardware state.
11. Push the resulting status to the user's web/mobile interface.

## 12. Reconnection and reconciliation

When the gateway reconnects:

1. Accept and authenticate the new WebSocket session.
2. Wait for its initial `state_snapshot`.
3. Update reported states from that snapshot.
4. Compare reported states with desired states.
5. Do not replay expired commands.
6. If product policy requires desired-state enforcement, create a new command ID and send a new, unexpired command.
7. Never override `fault` or `overcurrent` with automatic reconciliation.

The safest default is not to automatically replay an old ON command after a long outage. Require a new command or an explicitly configured reconciliation policy.

## 13. Server pseudocode

```text
on_websocket_upgrade(request):
    api_key = request.header("API-KEY")
    gateway_mac = request.query("device_id")

    authorize(api_key, gateway_mac)
    replace_existing_connection(gateway_mac)
    register_connection(gateway_mac, websocket)

on_websocket_message(gateway_mac, message):
    data = parse_json(message)

    if data.type == "state_snapshot":
        verify data.gateway_id == gateway_mac
        update_gateway_reported_state(data.gateway)
        upsert_socket_reported_states(data.sockets)
        update_last_seen(gateway_mac)

    else if data.type == "telemetry":
        verify data.gateway_id == gateway_mac
        persist_telemetry(gateway_mac, data.inverter_data)
        update_last_seen(gateway_mac)

    else if data.type == "command_result":
        verify data.gateway_id == gateway_mac
        command = find_command(data.command_id)
        verify command belongs to gateway_mac
        update_command_result(command, data)

        if data.result == "confirmed":
            update_target_reported_state(data.reported_state)

        publish_status_to_user_clients(command, data)

send_state_command(gateway_mac, target_type, target_id, desired_state):
    command = create_command(
        command_id = new_ulid(),
        expires_at = utc_now() + 30 seconds
    )

    websocket = active_connection(gateway_mac)
    if websocket is null:
        mark_command_offline(command)
        return

    websocket.send_json({
        "type": "set_state",
        "command_id": command.id,
        "target_type": target_type,
        "target_id": target_id,
        "desired_state": desired_state,
        "expires_at": unix_seconds(command.expires_at)
    })
```

## 14. Operational and security requirements

- Accept only `wss://`; do not expose an unencrypted production endpoint.
- Validate the `API-KEY` before upgrading the connection.
- Authorize the API key for the provided gateway MAC.
- Validate every JSON message and enforce length limits.
- Rate-limit commands per user, gateway, and socket.
- Audit user, target, requested state, command ID, and final outcome.
- Reject commands with invalid states or unknown targets before sending where possible.
- Never treat WebSocket delivery as hardware confirmation.
- Do not automatically bypass overcurrent or other local protection.
- Rotate credentials and support per-device revocation.

The firmware validates `api.acecore.tech` using the ISRG Root X1 trust anchor. If the backend hostname or certificate authority changes, update the firmware WebSocket configuration and CA trust anchor.

## 15. Current firmware limitations

- The endpoint `/ws/device` is assumed; the backend must implement it or the firmware configuration must be changed.
- Approved smart sockets are currently stored in gateway RAM and may need rediscovery/approval after a restart.
- Recently processed command IDs are kept in RAM and reset when the ESP32 restarts.
- `timestamp_ms` is uptime, not Unix time.
- The firmware reports `idle` only if that behavior is added later; currently it reports ON/OFF/switching/fault.
- The server must accept and persist `telemetry` messages from the device WebSocket.
- The current firmware flash usage is approximately 88%, so future additions should be mindful of firmware size.

## 16. Firmware implementation references

- WebSocket implementation: `src/websocket_commands.cpp`
- WebSocket public functions: `include/websocket_commands.h`
- Host and route configuration: `include/config.h`
- TLS trust anchor: `include/websocket_ca.h`
- Main startup/service integration: `src/main.cpp`
- Smart-socket acknowledgement integration: `src/espnow_handler.cpp`
- Main relay persistence and state application: `src/webinterface.cpp`
