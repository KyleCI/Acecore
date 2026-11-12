#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ESP32 self-update over HTTPS (ported from lib/PowercellOTA 1.0.4).
// Backend contract (official-powercell-app PR #3, FirmwareService):
//   POST {base}/api/ota/check {"device_id":"<12 hex>","firmware_version":"x.y.z"}
//   GET  <url from the check>  (= {base}/api/ota/download?device_id=<stored id>)
// Both with "Authorization: Bearer <API_KEY>" (shared hardware key), X-Device-ID,
// and X-Device-Secret when provisioned. The image must match the response
// sha256 (64 hex, required), the embedded kind/model/version identity, and
// the url must be on the configured base host and a documented download path.
// Rollback: the bootloader has APP_ROLLBACK enabled. A new image boots in
// PENDING_VERIFY and is marked valid only after OTA_HEALTHY_MS of continuous
// health (backend WebSocket connected). If it is not healthy within
// OTA_VERIFY_DEADLINE_MS, or it reboots/crashes first, it rolls back.
// hotspot_only (no cloud, so no WebSocket health): OTA_LAN_HEALTHY_MS of healthy local run
// (free heap > 30 KB) confirms instead, within the same OTA_VERIFY_DEADLINE_MS.
namespace ota {
void begin(bool (*prepareUpdate)(), bool (*acquireNetwork)());
void service(bool healthy);
bool busy();         // an image is being downloaded/installed
bool networkBusy();  // an install owns the network: the WebSocket is paused
bool pendingVerify();
bool confirmed();
const char *status();
const char *version();
void checkSoon();
bool tlsBusy();
// OTA check over the WS: hello_ack {ota_ws:bool}, and the replies ota_available / ota_none.
void wsSupport(bool otaWs);
void wsConnected();  // each WS (re)connect (fallback cycle count)
void wsDown();       // WS dropped: an outstanding check is deferred
void wsReply(JsonDocument &doc);
// Local hotspot OTA (POST /ota, from collector 2.5.12). Streamed via the Update lib.
// lanBegin/lanWrite/lanEnd return nullptr on success or a short error code; the HTTP status
// is in *httpCode. The image must match X-Firmware-SHA256 and carry
// POWERCELL_FW|collector|<this model>|<version>|END with a different version (older only with force).
// The boot partition is set only after both checks. Same rollback bookkeeping as the cloud path.
const char *lanBegin(const char *sha256Hex, bool force, int *httpCode);
const char *lanWrite(const uint8_t *data, size_t len, int *httpCode);
const char *lanEnd(int *httpCode, String *version);
void lanAbort(const char *why);
bool lanActive();
}  // namespace ota
