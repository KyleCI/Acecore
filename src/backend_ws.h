#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Persistent backend channel. Default host is the live cloud
// wss://api-dev.acecore.tech/ws/device (POWERCELL_WS_HOST). Production
// api.acecore.tech is the esp32dev_prod env only. Query: device_id=<MAC with colons>
// Handshake headers: API-KEY (unchanged) and X-Device-ID (12-hex MAC, added).
// Messages out: telemetry (5 s), state_snapshot (30 s + on change), command_result,
// pong, offline_sync. Messages in: ping, set_state, socket_lifecycle, offline_sync_ack.
namespace backend {
void begin();
// The WS (TCP + TLS handshake + ws.loop) runs in its own task ("ws", core 1) so a slow
// handshake (about 6 s at -83 dBm) no longer stalls the main loop. service() is that
// task's body; do not call it from loop().
void service();
// Recursive app lock: the main loop holds it for each pass; the WS task holds it while
// connected (frame handlers, telemetry) and in its event callback, never during a connect.
void appLock();
void appUnlock();
bool connected();
// Healthy = connected and accepted by the backend for the OTA health check.
bool healthy();
bool send(JsonDocument &doc);
void addIdentity(JsonDocument &doc);
void publishState();
// OTA needs the heap held by the WS TLS session; called on the main loop.
bool pauseForOta();
void requestTelemetry();  // send telemetry at the next service() (new event)
bool sendOtaStatus(const char *status, const char *version);
bool sendOtaCheck(bool healthy, const char *lastStatus, const char *lastError);  // {type:ota_check, fw, kind, board}
uint32_t reconnects();
uint32_t connectedForMs();  // 0 when not connected
}  // namespace backend
