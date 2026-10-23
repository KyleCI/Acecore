#include <PowercellOTA.h>
#include "websocket_commands.h"
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <time.h>
#include "collector_lora.h"
#include "websocket_ca.h"
#include "offline_sync.h"

namespace {
#ifndef POWERCELL_WS_HOST
#define POWERCELL_WS_HOST "api.acecore.tech"
#endif
#ifndef POWERCELL_WS_PORT
#define POWERCELL_WS_PORT 443
#endif
#ifndef POWERCELL_WS_TLS
#define POWERCELL_WS_TLS 1
#endif

constexpr const char *HOST = POWERCELL_WS_HOST;
constexpr uint16_t PORT = POWERCELL_WS_PORT;
constexpr const char *PATH = "/ws/device";
constexpr const char *GATEWAY_NAME = "ACE1100";
constexpr size_t MAX_MESSAGE = 2048;
WebSocketsClient ws;
String authenticationHeader;
String deviceApiKey;
bool initialized = false, started = false;
volatile bool connected = false;
unsigned long lastSnapshot = 0, lastTelemetry = 0;

struct PendingCommand {
  bool active = false;
  String id, deviceId;
  bool desired = false;
  unsigned long deadline = 0, lastAttempt = 0;
} pending;

uint64_t unixNow() {
  time_t now = time(nullptr);
  return now >= 1700000000 ? static_cast<uint64_t>(now) : 0;
}

String urlEncode(const String &value) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < value.length(); ++i) {
    uint8_t c = value[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

void identity(JsonDocument &doc) {
  doc["firmware_version"] = PowercellOTA::version();
  doc["hardware_model"] = POWERCELL_HARDWARE_MODEL;
  doc["gateway_id"] = WiFi.macAddress();
  doc["timestamp"] = unixNow();
  doc["timestamp_ms"] = millis();
}

bool sendJson(JsonDocument &doc) {
  if (!connected) return false;
  String json;
  serializeJson(doc, json);
  return json.length() < MAX_MESSAGE && ws.sendTXT(json);
}

void result(const String &id, const char *targetType, const String &targetId,
            bool desired, const char *status, const char *error = nullptr) {
  JsonDocument doc;
  doc["type"] = "command_result";
  identity(doc);
  doc["command_id"] = id;
  doc["target_type"] = targetType;
  doc["target_id"] = targetId;
  doc["desired_state"] = desired ? "on" : "off";
  doc["reported_state"] = desired ? "on" : "off";
  doc["result"] = status;
  if (error) { doc["error"] = error; doc["reason"] = error; }
  sendJson(doc);
}

void snapshot() {
  JsonDocument doc;
  doc["type"] = "state_snapshot";
  identity(doc);
  doc["gateway_name"] = GATEWAY_NAME;
  doc["device_name"] = GATEWAY_NAME;
  doc["online"] = true;
  JsonObject gateway = doc["gateway"].to<JsonObject>();
  gateway["reported_state"] = deviceState ? "on" : "off";
  gateway["status"] = deviceState ? "on" : "off";
  JsonArray sockets = doc["sockets"].to<JsonArray>();
  populateDeviceArray(sockets);
  sendJson(doc);
  lastSnapshot = millis();
}

void telemetry() {
  JsonDocument doc;
  doc["type"] = "telemetry";
  identity(doc);
  JKPacketData bms;
  if (getBmsData(bms)) {
    JsonObject battery = doc["battery"].to<JsonObject>();
    battery["voltage"] = bms.packVoltage;
    battery["current"] = bms.packCurrent;
    battery["power"] = bms.packPower;
    battery["soc"] = bms.SOC;
    battery["temperature"] = bms.tempBat1;
  }
  InverterSnapshot inv = getInverterStatus();
  JsonObject inverter = doc["inverter"].to<JsonObject>();
  inverter["system"] = inv.sys;
  inverter["switch"] = inv.sw;
  inverter["grid_voltage"] = inv.gridVoltage;
  inverter["output_voltage"] = inv.vout;
  inverter["output_current"] = inv.iout;
  inverter["temperature"] = inv.tempC;

  JsonObject solar = doc["solar"].to<JsonObject>();
  MpptData mppt;
  if (mppt_get_data(mppt)) {
    solar["voltage"] = mppt.v_bus_V;
    solar["current"] = mppt.current_A;
    solar["power"] = mppt.power_W;
  }
  JsonArray sockets = doc["sockets"].to<JsonArray>();
  populateDeviceArray(sockets);
  sendJson(doc);
  lastTelemetry = millis();
}

void reject(const String &id, const String &kind, const String &target,
            bool desired, const char *reason) {
  result(id, kind.c_str(), target, desired, "rejected", reason);
}

void incoming(uint8_t *payload, size_t length) {
  if (!payload || !length || length > MAX_MESSAGE) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) || !doc.is<JsonObject>()) return;
  String type = doc["type"] | "";
  if (type == "ping") {
    JsonDocument pong; pong["type"] = "pong"; identity(pong); sendJson(pong); return;
  }
  if (type == "offline_sync_ack") {
    acknowledgeOfflineEvents(doc["event_ids"].as<JsonArrayConst>());
    return;
  }
  if (type != "set_state") return;
  String id = doc["command_id"] | "";
  String kind = doc["target_type"] | "";
  String target;
  if (doc["target_id"].is<const char *>()) target = doc["target_id"].as<String>();
  else if (doc["target_id"].is<int>()) target = String(doc["target_id"].as<int>());
  String desiredText = doc["desired_state"] | "";
  bool desired = desiredText == "on";
  if (!id.length() || id.length() > 47) { reject(id, kind, target, desired, "invalid_command_id"); return; }
  if (desiredText != "on" && desiredText != "off") { reject(id, kind, target, false, "invalid_state"); return; }
  if (kind != "gateway" && kind != "socket") { reject(id, kind, target, desired, "invalid_target_type"); return; }
  uint64_t expires = doc["expires_at"] | uint64_t(0);
  if (expires && unixNow() && expires <= unixNow()) { reject(id, kind, target, desired, "command_expired"); return; }

  if (firmwareOTA.busy()) { reject(id, kind, target, desired, "firmware_update_in_progress"); return; }
  if (kind == "gateway") {
    if (target != GATEWAY_NAME && !target.equalsIgnoreCase(WiFi.macAddress())) {
      reject(id, kind, target, desired, "unknown_target"); return;
    }
    applyDeviceState(desired);
    result(id, "gateway", target, desired, "confirmed");
    snapshot();
  } else {
    if (!target.length() || !checkRegistered(target)) {
      reject(id, kind, target, desired, "unknown_target"); return;
    }
    if (pending.active) { reject(id, kind, target, desired, "command_queue_full"); return; }
    pending.active = true;
    pending.id = id;
    pending.deviceId = target;
    pending.desired = desired;
    pending.deadline = millis() + 7000;
    pending.lastAttempt = millis();
    sendRelayCommand(target, desired);
    snapshot();
  }
}

void event(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    connected = true;
    Serial.println("[WS] Secure WebSocket connected");
    snapshot(); telemetry();
  } else if (type == WStype_DISCONNECTED) {
    connected = false;
    Serial.println("[WS] Disconnected; reconnect scheduled");
  } else if (type == WStype_TEXT) incoming(payload, length);
  else if (type == WStype_ERROR) {
    connected = false;
    Serial.printf("[WS] Transport/TLS error: %.*s\n", int(length), payload ? reinterpret_cast<char *>(payload) : "");
  }
}
} // namespace

void initWebSocketCommands() {
  if (initialized) return;
  Preferences credentials;
  credentials.begin("powercell_ota", false);
  deviceApiKey = credentials.getString("api_key");
  if (deviceApiKey.isEmpty() && POWERCELL_API_KEY[0]) {
    deviceApiKey = POWERCELL_API_KEY;
    credentials.putString("api_key", deviceApiKey);
  }
  credentials.end();
  if (deviceApiKey.isEmpty()) { Serial.println("[WS] API key missing; provision OTA_CONFIG api_key over USB and reboot"); return; }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  ws.onEvent(event);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(15000, 3000, 2);
  initialized = true;
}

void serviceWebSocketCommands() {
  if (!initialized || WiFi.status() != WL_CONNECTED) { connected = false; return; }
  if (!started) {
    if (!unixNow()) return;
    String path = String(PATH) + "?device_id=" + urlEncode(WiFi.macAddress());
    // WebSocketsClient stores this pointer for the later asynchronous
    // handshake, so the backing String must live for the whole connection.
    authenticationHeader = String("API-KEY: ") + deviceApiKey;
    ws.setExtraHeaders(authenticationHeader.c_str());
#if POWERCELL_WS_TLS
    ws.beginSslWithCA(HOST, PORT, path.c_str(), WEBSOCKET_CA_CERT, "arduino");
#else
    ws.begin(HOST, PORT, path.c_str(), "arduino");
#endif
    started = true;
    Serial.printf("[WS] Connecting to %s://%s:%u%s\n",
                  POWERCELL_WS_TLS ? "wss" : "ws", HOST, PORT, path.c_str());
  }
  ws.loop();
  if (!connected) return;
  unsigned long now = millis();
  if (now - lastTelemetry >= 5000) telemetry();
  if (now - lastSnapshot >= 30000) snapshot();
  if (pending.active
      && static_cast<long>(now - pending.deadline) < 0
      && now - pending.lastAttempt >= 1500) {
    sendRelayCommand(pending.deviceId, pending.desired);
    pending.lastAttempt = now;
  }
  if (pending.active && static_cast<long>(now - pending.deadline) >= 0) {
    result(pending.id, "socket", pending.deviceId, pending.desired, "rejected", "hardware_timeout");
    pending.active = false;
    snapshot();
  }
}

bool webSocketCommandsConnected() { return initialized && connected; }
bool publishWebSocketTelemetry(const String &json) {
  if (!connected || json.length() >= MAX_MESSAGE) return false;
  String message = json;
  return ws.sendTXT(message);
}
void notifyWebSocketSocketResult(const String &deviceId, bool relayOn) {
  if (!pending.active || pending.deviceId != deviceId) return;
  // A telemetry frame carrying the previous relay state can arrive while the
  // LoRa command is still in flight. Only the requested state is a terminal
  // acknowledgement; otherwise keep retrying until hardware_timeout.
  if (relayOn != pending.desired) return;
  result(pending.id, "socket", deviceId, pending.desired, "confirmed");
  pending.active = false;
  snapshot();
}
void publishWebSocketState() { if (connected) snapshot(); }
