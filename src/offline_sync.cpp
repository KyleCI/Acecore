#include "offline_sync.h"
#include "websocket_commands.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
namespace {
Preferences store;
String queueJson = "[]";
unsigned long lastAttempt = 0;
constexpr size_t MAX_EVENTS = 24;
void save(JsonArrayConst events) {
  String next; serializeJson(events, next); queueJson = next;
  store.begin("offline_sync", false); store.putString("events", queueJson); store.end();
}
}
void initOfflineSync() {
  store.begin("offline_sync", true); queueJson = store.getString("events", "[]"); store.end();
  JsonDocument check;
  if (deserializeJson(check, queueJson) || !check.is<JsonArray>()) queueJson = "[]";
  Serial.printf("[Sync] Restored %u offline events\n", pendingOfflineEventCount());
}
void queueOfflineEvent(const String &action, const String &socketId, const String &value) {
  JsonDocument doc; deserializeJson(doc, queueJson); JsonArray events = doc.as<JsonArray>();
  while (events.size() >= MAX_EVENTS) events.remove(0);
  JsonObject event = events.add<JsonObject>();
  String mac = WiFi.macAddress(); mac.replace(":", "");
  event["event_id"] = mac + "-" + String(millis(), HEX) + "-" + String(esp_random(), HEX);
  event["action"] = action;
  if (socketId.length()) event["socket_id"] = socketId;
  if (value.length()) event["value"] = value;
  event["created_ms"] = millis();
  save(events);
  Serial.printf("[Sync] Queued %s for %s (%u pending)\n", action.c_str(), socketId.c_str(), events.size());
}
void serviceOfflineSync() {
  if (!webSocketCommandsConnected() || millis() - lastAttempt < 2000 || queueJson == "[]") return;
  lastAttempt = millis();
  JsonDocument queued;
  if (deserializeJson(queued, queueJson) || !queued.is<JsonArray>()) return;
  JsonDocument envelope;
  envelope["type"] = "offline_sync";
  envelope["gateway_id"] = WiFi.macAddress();
  envelope["gateway_name"] = "ACE1100";
  envelope["events"] = queued.as<JsonArrayConst>();
  String json; serializeJson(envelope, json); publishWebSocketTelemetry(json);
}
void acknowledgeOfflineEvents(JsonArrayConst ids) {
  JsonDocument queued; deserializeJson(queued, queueJson);
  JsonDocument retained; JsonArray next = retained.to<JsonArray>();
  for (JsonObjectConst event : queued.as<JsonArray>()) {
    bool acked = false;
    for (JsonVariantConst id : ids) if (id.as<String>() == event["event_id"].as<String>()) { acked = true; break; }
    if (!acked) next.add(event);
  }
  save(next); Serial.printf("[Sync] Server acknowledged events; %u pending\n", next.size());
}
size_t pendingOfflineEventCount() {
  JsonDocument doc; return deserializeJson(doc, queueJson) ? 0 : doc.as<JsonArray>().size();
}
