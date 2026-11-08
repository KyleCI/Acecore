# Local login and Manual settings

Pinned against app PR 8 build 73, commit `2cee8e6c0e71efd1fa8299c7445b4e6949407b05`,
and checked against collector 2.5.11 archive `4f39c95`. The app repository was
not readable from this run. The login fields below are the ones that archive
already returns and that the extraction says the app parses. The settings
routes are new: that app still posts Manual settings only to the cloud
(`POST /optimization/default/settings`). It has no local settings client.

Nothing here was installed on a unit. A phone that fails before response
headers is a separate capture and is not claimed fixed.

## POST /v2/login

Unauthenticated. `Content-Type: application/json`. `Accept` may be
`application/json` or `*/*`. All four values are JSON strings.

```json
{"device_id":"aa:bb:cc:dd:ee:ff","passcode":"135790","phone_id":"11111111-2222-3333-4444-555555555555","phone_name":"Powercore"}
```

`device_id` is the string from `GET /v2/setup/info`. Matching ignores case
and `:` / `-` in a MAC. `phone_id` is the app's stable onboarding UUID.
`phone_name` in the extracted client is the literal `Powercore`.

Success is one JSON object, HTTP 200. `lan_token` is always present and
non-empty. `token` and `access_token` are not sent. `expires_in` is the
integer 1800. `passcode_default` is a JSON boolean. After a real setup it
is `false`. Only the boolean `true` means a default passcode.

```json
{"session":"<48 hex>","expires_in":1800,"lan_token":"<48 hex>","passcode_default":false}
```

The app needs `lan_token` to open Home. It stores that value, not `session`,
as `Authorization: Bearer` for later calls. A lost response is not replayed
by the app. A failed login does not clear saved LAN tokens. A successful
login replaces only that `phone_id`'s token. There is no retry cache. The
passcode check is PBKDF2 and is meant to finish well inside the app's 12
second header-plus-body wait. This was not timed on a device.

Errors:

| HTTP | Body | When |
| --- | --- | --- |
| 401 | `{"error":"bad_credentials"}` | Wrong passcode, unknown device id, or no passcode yet. `retry_after` is added only when a lockout has just started. |
| 429 | `{"error":"locked","retry_after":30}` | Already locked. `retry_after` is an integer. |
| 400 | `{"error":"invalid_request"}` | Missing string field. |
| 415 | `{"error":"unsupported_media"}` | `Content-Type` is not JSON. |
| 500 | `{"error":"storage_failed"}` | The pair store did not commit. The previous token remains. |

`API-KEY` and the OTA bearer are not login credentials and are not accepted
as the saved LAN token.

## GET /v2/setup/info

No authentication. `passcode_set` is the boolean the app should trust.
`same_as_label` is not sent. `powercell_model`, `rated_kva`, and `usable_kw`
are JSON null. This firmware does not invent a usable rating.

```json
{"device_id":"ACE-DDEEFF","model":"hw-v0.1.1","fw":"1.0.1","state":"unconfigured","claimed":false,"passcode_default":false,"passcode_set":false,"hotspot":false,"onboarding_version":2,"powercell_model":null,"rated_kva":null,"usable_kw":null}
```

`state` is `configured` only when a Wi-Fi SSID is saved. The first passcode
is `POST /v2/setup/passcode` on the hotspot, the same four-string body as
login without `device_id`. That route is how a unit with `passcode_set:false`
gets a token. The extracted app notes do not show it calling this route.

## GET /v2/status

`Authorization: Bearer` may be the login `session` or the saved `lan_token`.
Anything else, including `API-KEY`, is HTTP 401 `{"error":"unauthorized"}`.

```json
{"device_id":"ACE-DDEEFF","fw":"1.0.1","hotspot":false,"sta_connected":false,"payment_locked":false,"payment_lock_reason":"","cloud_linked":false,"auth":"session"}
```

`auth` is `session` or `lan` so a test can see which credential matched.
The app's status parser beyond that choice was not in the extraction.

## Local Manual settings

These routes are not in app PR 8. They are the firmware contract for a
future local Save. Bearer is the saved `lan_token` only. The login `session`
is rejected. There is no cloud session and no `/optimization/default/settings`
on the device.

### Battery

`GET` or `POST /v1/battery-settings`

```json
{"schema":1,"low_percent":30,"warning_percent":18,"base_revision":0}
```

Applied response:

```json
{"schema":1,"result":"applied","reason":"none","applied":true,"durable_commit":true,"pending":false,"physical_shutdown_verified":false,"readback":{"schema":1,"low_percent":30,"warning_percent":18,"inhibit_percent":10,"restart_percent":15,"applied_revision":1,"durable":true}}
```

`GET` uses `"result":"readback"` and `"applied":false`. The readback is the
canonical stored value. A later Save must send that `applied_revision` as
`base_revision`. A stale revision is HTTP 400 `reason":"stale_revision"` and
does not change the store. A non-integer percent is HTTP 400
`reason":"not_integer"`. Warning must be 11..80. Low must be 12..95 and
greater than the warning. 10 and 15 are not writable. HTTP 401 has
`readback_present:false` and does not return the stored numbers.

After reboot the `lan_token` still reads the same revision. The `session`
does not survive reboot.

### System rating

`GET` or `POST /v1/system-rating` with the saved `lan_token`. This is rated
active watts, not kVA. The only accepted values are 5000 and 3500. Usable
power is 90 percent and is not writable: 4500 W and 3150 W. Any other number,
including a float, is `unknown_rating` or `not_integer` and is not stored.
`usable_kw` on setup info stays null. There is no rating for an unknown system.

```json
{"schema":1,"rated_active_w":5000,"base_revision":0}
```

```json
{"schema":1,"result":"applied","reason":"none","applied":true,"durable_commit":true,"pending":false,"physical_shutdown_verified":false,"readback":{"rated_active_w":5000,"usable_active_w":4500,"applied_revision":1}}
```

`GET /v2/status` then includes `rated_active_w`, `usable_active_w`, and
`overload_configured`. Before a rating they are null and false.
`total_output_coverage_verified` stays false.

The system overload compares fresh PZEM active watts with that usable value.
It is not the old 19 A STM32 rule. A sample at the usable value does not trip.
One watt above it, after a 3 second startup grace and 8 seconds sustained,
requests AC output off. A stale or invalid sample does not trip and does not
clear a latch. Clearing the latch does not turn the output on. This is
software using the meter. It is not a separate hardware breaker.

## AC ceiling

`GET` or `POST /v1/ac-output-settings`

```json
{"schema":1,"ceiling_w":500,"mode":"warn_and_shutdown","base_revision":0}
```

`mode` is `warn_only` or `warn_and_shutdown`. The ceiling must be an integer
from 50 to the configured usable watts. A higher ceiling is
`ceiling_above_usable` and does not change the system limit. `warn_only`
does not shut down at the ceiling. The usable-power overload still does.
A battery POST cannot set or clear either value.

A battery-only body does not change the AC ceiling. `ceiling_w: 0` in a battery POST is ignored. `30/18` is one legal sample, not the only accepted pair. Defaults before any save are low 25 and warning 15. Inhibit 10 and restart 15 are read-only.

A response is a successful Save only when HTTP is 200 and the body has `result` `applied`, `applied` true, `durable_commit` true, and a following GET returns the same `applied_revision` and values. HTTP 200 is not used for a rejected save. A timeout or a body without those fields is not success.

## POST /state and POST /relay

`/state` is the collector output. Authorization may be the login session or the saved LAN token. `{"state":"off"}` on v0.1.1 drives GPIO15 high (DSP off) and does not address an AcePlug. `{"state":"on"}` drives GPIO15 low unless the battery latch or a payment lock refuses it. The original profile uses its existing active-high collector relay, not GPIO15-as-DSP.

```json
{"ok":true,"state":"off","applied_via":"dsp_gpio15","aceplug":false,"gpio_level":1}
```

`/relay` is an AcePlug socket command. On v0.1.1 there is no LoRa radio, so the response is HTTP 409 and GPIO15 is not written:

```json
{"ok":false,"error":"lora_not_fitted","aceplug":true,"dsp_gpio15":false}
```

On the original profile a registered socket is queued as `RELAY_ON` / `RELAY_OFF` and returns HTTP 202. That path does not drive the v0.1.1 DSP pin.

## Other app paths

`/data` with no `Authorization` header stays open for the on-device page.
A presented bearer must be the saved `lan_token`. A session, `API-KEY`, or
OTA bearer is HTTP 401. `/events` stays open because a browser EventSource
cannot send that header. `POST /state` and `POST /relay` are implemented
and described above. They are not missing.

Battery and AC settings use the same rejection for a login session:
HTTP 401, `result` `rejected`, `reason` `session_not_sufficient`,
`applied` false. Missing credentials, `API-KEY`, and an OTA bearer use
`reason` `unauthorized`. Neither case is a successful Save.

## Not ported

The 19 A inverter overload in 2.5.11 is an STM32 output-current rule. v0.1.1
does not use it. Its overload is the setup usable watts against fresh PZEM
active watts. The original profile has no PZEM and does not run that path.

`pc_ident/pay_lock`, when that key is already true, still refuses a user ON
or OFF. Battery cutoff and OTA off still run. This firmware does not contain
the command that sets the lock. There is no payment-lock actuator beyond that
flag. `physical_shutdown_verified` remains false.
