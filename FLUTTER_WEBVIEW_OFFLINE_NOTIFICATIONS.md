> **Firmware 1.0.1 reference.** The collector on this branch is firmware **2.5.0**.
> This file is kept from Dubem's tree. For the current collector (app `/v2/*`, `api.acecore.tech` vs `api-dev.acecore.tech`, factory identity), start at [README.md](README.md).

# Flutter WebView Offline Notifications Integration

This document defines how the PowerCell Flutter application should consume local
notifications directly from the ESP32 collector when internet access is not
available.

## 1. Scope and limitations

The phone and ESP32 must be reachable on the same local network. The current
collector address is:

```text
http://192.168.1.100
```

Offline notifications are reliable while the app is open and its WebView is
running. Android or iOS may suspend networking after the app enters the
background. A terminated app cannot be reliably awakened by the ESP32 without
an operating-system-supported background mechanism or internet push service.

## 2. Interfaces exposed by the ESP32

### Live event stream

```http
GET /events
Accept: text/event-stream
```

Full URL:

```text
http://192.168.1.100/events
```

The endpoint uses Server-Sent Events (SSE). It is the primary interface for
immediate notifications while the WebView is active.

### State and recovery endpoint

```http
GET /data
Accept: application/json
```

Full URL:

```text
http://192.168.1.100/data
```

Use `/data` when initially connecting, after an SSE reconnect, and as a polling
fallback. It contains current device state and currently active notifications.

## 3. SSE event types

The `/events` stream emits named events.

| Event | Purpose |
|---|---|
| `ready` | Confirms that the SSE client connected. |
| `state` | Complete `/data`-style state snapshot. |
| `notification` | A notification transition requiring app processing. |
| `heartbeat` | Sent approximately every 15 seconds to prove connectivity. |

### Ready event

```json
{
  "status": "connected"
}
```

### Heartbeat event

```json
{}
```

Record the receipt time in Flutter. If no event or heartbeat is received for
approximately 35 seconds, consider the collector connection stale and start
recovery polling.

## 4. Notification schema

Notification events and entries in `/data.notifications` use this common shape:

```json
{
  "id": "socket_3_overcurrent",
  "type": "overcurrent",
  "severity": "critical",
  "title": "Socket overcurrent",
  "message": "Socket 3 tripped at 12.4 A",
  "active": true,
  "sequence": 2,
  "sound": "critical",
  "socket_id": 3,
  "dummy": false
}
```

Fields:

| Field | Type | Meaning |
|---|---:|---|
| `id` | string | Stable identity for the notification condition. |
| `type` | string | Machine-readable notification category. |
| `severity` | string | `info`, `warning`, or `critical`. |
| `title` | string | Short title suitable for a local notification. |
| `message` | string | Human-readable detail. |
| `active` | boolean | `true` activates the condition; `false` clears it. |
| `sequence` | integer | Changes whenever that condition transitions. |
| `sound` | string | `none`, `info`, `warning`, or `critical`. |
| `socket_id` | integer | Present for socket-related notifications. |
| `dummy` | boolean | When true, the ESP32 is using temporary simulated policy data. |

The app must tolerate additional fields and missing optional fields.

## 5. Supported notification types

| Type | Trigger | Status |
|---|---|---|
| `battery_low` | Battery at or below 25% | Production sensor reading |
| `battery_low` critical | Battery at or below 10% | Production sensor reading |
| `battery_full` | Battery at or above 95% | Dummy policy threshold |
| `shutdown_imminent` | Battery at or below 5% | Dummy notification only |
| `overcurrent` | Smart socket reports an overcurrent trip/clear | Hardware event |
| `relay_failed` | Send failure, timeout, state mismatch, lockout, or conflict | Hardware/command event |

`shutdown_imminent` does not mean the ESP32 performed a physical shutdown. Its
payload contains `"dummy": true` until a BMS or power-controller shutdown signal
is integrated. The app may display it as a warning but must not display
"PowerCell shut down successfully."

## 6. Deduplication rules

Use this key for every notification:

```text
<collector device ID>:<notification id>:<sequence>
```

For example:

```text
C8:85:41:2C:31:D4:socket_3_overcurrent:2
```

Persist processed keys in Flutter local storage. Do not show the same key more
than once. A new `sequence` represents a new transition and may be displayed.

When `active` is false:

- cancel or update the corresponding ongoing notification;
- store the new sequence;
- optionally show a low-priority recovery notification.

Do not deduplicate only by `id`, because the same condition can trip, recover,
and trip again.

## 7. Recommended WebView architecture

Load a page served by the ESP32, such as:

```text
http://192.168.1.100/monitor
```

This keeps the page and `/events` on the same origin. The Flutter WebView should
register a JavaScript channel named:

```text
PowerCellNotifications
```

JavaScript forwards validated ESP32 notifications to Flutter using:

```javascript
PowerCellNotifications.postMessage(JSON.stringify(payload));
```

Flutter parses the message, deduplicates it, updates application state, and
invokes the local-notification plugin. Do not allow arbitrary page content to
invoke privileged native actions; only load trusted local PowerCell pages.

## 8. WebView JavaScript event client

The following behavior should be included in the WebView page or injected after
the page finishes loading:

```javascript
(() => {
  const processed = new Set();
  let source = null;
  let fallbackTimer = null;
  let lastEventAt = Date.now();

  function bridge(payload) {
    if (!window.PowerCellNotifications?.postMessage) return;
    window.PowerCellNotifications.postMessage(JSON.stringify(payload));
  }

  function notificationKey(item) {
    return `${item.id}:${item.sequence}`;
  }

  function processNotification(item, origin) {
    if (!item || typeof item.id !== 'string') return;
    if (!Number.isInteger(Number(item.sequence))) return;

    const key = notificationKey(item);
    if (processed.has(key)) return;
    processed.add(key);

    bridge({
      channel: 'notification',
      origin,
      received_at: new Date().toISOString(),
      payload: item
    });
  }

  async function recoverFromData() {
    try {
      const response = await fetch('/data', { cache: 'no-store' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const state = await response.json();

      for (const item of state.notifications || []) {
        processNotification(item, 'data');
      }

      bridge({ channel: 'state', origin: 'data', payload: state });
    } catch (error) {
      bridge({
        channel: 'connection',
        status: 'offline',
        message: String(error)
      });
    }
  }

  function connect() {
    source?.close();
    source = new EventSource('/events');

    source.addEventListener('ready', event => {
      lastEventAt = Date.now();
      bridge({ channel: 'connection', status: 'connected' });
      recoverFromData();
    });

    source.addEventListener('heartbeat', () => {
      lastEventAt = Date.now();
    });

    source.addEventListener('state', event => {
      lastEventAt = Date.now();
      try {
        bridge({ channel: 'state', origin: 'sse', payload: JSON.parse(event.data) });
      } catch (_) {}
    });

    source.addEventListener('notification', event => {
      lastEventAt = Date.now();
      try {
        processNotification(JSON.parse(event.data), 'sse');
      } catch (_) {}
    });

    source.onerror = () => {
      bridge({ channel: 'connection', status: 'reconnecting' });
      recoverFromData();
    };
  }

  fallbackTimer = setInterval(() => {
    if (Date.now() - lastEventAt > 35000) recoverFromData();
  }, 10000);

  window.addEventListener('beforeunload', () => {
    source?.close();
    clearInterval(fallbackTimer);
  });

  connect();
})();
```

The JavaScript `Set` only deduplicates during the current page session. Flutter
must also persist deduplication keys so WebView reloads do not repeat alerts.

## 9. Flutter message handling

Configure the WebView with JavaScript enabled and register the
`PowerCellNotifications` JavaScript channel before loading the page.

For every channel message:

1. Decode the outer JSON object.
2. Switch on `channel`:
   - `notification`: validate and display/cancel a local notification.
   - `state`: update the app's current PowerCell state.
   - `connection`: update the local/offline connection indicator.
3. For notification messages, build the persistent deduplication key.
4. Ignore the event if that key was already processed.
5. Save the key before displaying the notification.
6. Map `severity` and `sound` to the appropriate local-notification channel.

Suggested Android notification channels:

| Channel | Importance | Sound |
|---|---|---|
| PowerCell information | Default | Optional |
| PowerCell warnings | High | Warning sound |
| PowerCell critical alerts | Max/High | Critical alarm sound |

On iOS, request notification permission before attempting to display alerts.
Do not repeatedly request permission after the user declines.

## 10. App lifecycle

### Foreground/resumed

- Resume or reload the WebView.
- Reconnect `/events`.
- Immediately fetch `/data`.
- Process active notifications using persisted deduplication keys.

### Background/paused

- Expect SSE to stop eventually.
- Do not mark the ESP32 offline immediately.
- Record the time the app paused.

### Returned to foreground

- Fetch `/data` before showing a healthy connection state.
- Reconcile relay, overcurrent, battery, and active notification state.
- Restart SSE.

### Terminated

No reliable offline notification delivery is guaranteed. The next app launch
must call `/data` and reconcile the current state.

## 11. Platform configuration

### Android

The ESP32 endpoint currently uses plain local HTTP. Android may block cleartext
traffic unless the application permits it for the local collector connection.
Restrict any cleartext exception as narrowly as the Android networking stack and
application architecture allow. Request notification permission on Android 13+
and ensure the app has network access.

### iOS

Declare local-network access with a user-facing explanation. Configure App
Transport Security appropriately for the local HTTP device. iOS may show a
Local Network permission prompt the first time the app accesses the collector.
If the user denies it, `/data` and `/events` will be unreachable.

### WebView security

- Do not enable universal file access.
- Do not load untrusted remote content into a WebView that exposes the native
  notification JavaScript channel.
- Do not place API keys in WebView JavaScript.
- The local `/data` and `/events` endpoints currently have no user
  authentication; treat access to the local Wi-Fi network as sensitive.

## 12. Testing checklist

- Connect the app and confirm receipt of `ready` and `state`.
- Confirm a heartbeat arrives approximately every 15 seconds.
- Trigger low and critical battery test conditions.
- Trigger overcurrent and overcurrent-clear events.
- Trigger relay send failure, timeout, and state mismatch.
- Verify `active:false` clears an ongoing alert.
- Reload the WebView and verify notifications are not duplicated.
- Interrupt SSE and verify `/data` recovery works.
- Disconnect Wi-Fi and verify the app shows local collector unavailable.
- Reconnect Wi-Fi and verify state reconciliation.
- Confirm payloads marked `dummy:true` are labeled as simulated in diagnostic
  builds and never presented as confirmed physical shutdown actions.

## 13. Future production work

Replace the dummy battery-full threshold with a BMS charge-state signal. Replace
the dummy shutdown warning with a real power-controller lifecycle containing at
least `shutdown_imminent`, output isolation, `shutdown_started`, and a confirmed
low-power/deep-sleep transition.

For notifications while the app is terminated, add an internet-backed service
such as Firebase Cloud Messaging/APNs or implement a platform-specific local
background strategy with the understanding that iOS execution remains limited.
