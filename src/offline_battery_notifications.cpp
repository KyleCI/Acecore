#include "offline_battery_notifications.h"
#include "battery_alert_policy.h"
#include "bms.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <esp_system.h>

namespace {
using powercell::BatteryAlertLevel;
using powercell::BatteryAlertPolicy;
using powercell::BatteryReading;

AsyncEventSource events("/events");
BatteryAlertPolicy policy;
SemaphoreHandle_t snapshotMutex = nullptr;
String collectorId;
String snapshotJson;
String batteryJson;
String unavailableJson;
uint64_t sequence = 0;
uint64_t batterySequence = 0;
uint64_t unavailableSequence = 0;
uint32_t lastSampleAt = 0;
uint32_t lastHeartbeatAt = 0;
bool sampled = false;
bool persistentBootSequence = false;

void reserveBootSequence() {
  // One NVS write per boot, never per sample. Reserve 2^32 sequence numbers
  // so app deduplication survives reboots without needing internet/NTP time.
  Preferences prefs;
  uint32_t boot = 0;
  if (prefs.begin("battery_alerts", false)) {
    boot = prefs.getUInt("boot", 0) + 1;
    if (boot >= 0x200000 || prefs.putUInt("boot", boot) == 0) boot = 0;
    prefs.end();
  }
  persistentBootSequence = boot != 0;
  if (!persistentBootSequence) {
    // NVS failure must not disable monitoring. A random boot namespace keeps
    // duplicate risk small; log explicitly that persistence needs attention.
    boot = (esp_random() & 0x1fffff) | 1;
    Serial.println("[Battery alerts] NVS unavailable; using random boot sequence");
  }
  sequence = static_cast<uint64_t>(boot) << 32;
  batterySequence = ++sequence;
  unavailableSequence = ++sequence;
}

String alertJson(bool availability, const BatteryReading& reading) {
  JsonDocument doc;
  doc["collector_id"] = collectorId;
  doc["id"] = availability ? "battery_reading_unavailable" : "battery_low";
  doc["sequence"] = availability ? unavailableSequence : batterySequence;
  doc["sequence_persistent"] = persistentBootSequence;
  doc["dummy"] = false;
  if (availability) {
    doc["type"] = "battery_reading_unavailable";
    doc["active"] = policy.unavailableAlert();
    doc["severity"] = "warning";
    doc["sound"] = policy.unavailableAlert() ? "warning" : "none";
    doc["title"] = policy.unavailableAlert() ? "Battery reading unavailable" : "Battery readings restored";
    doc["message"] = policy.unavailableAlert()
        ? "No fresh, valid battery reading is available from the BMS."
        : "Fresh battery readings are available again.";
  } else {
    const auto level = policy.level();
    const bool active = level != BatteryAlertLevel::None;
    const bool critical = level == BatteryAlertLevel::Critical || level == BatteryAlertLevel::ShutdownRisk;
    doc["type"] = level == BatteryAlertLevel::ShutdownRisk ? "shutdown_imminent"
        : (level == BatteryAlertLevel::Critical ? "battery_critical" : "battery_low");
    doc["active"] = active;
    doc["severity"] = critical ? "critical" : (active ? "warning" : "info");
    doc["sound"] = active ? (critical ? "critical" : "warning") : "none";
    doc["title"] = !policy.available() ? "Battery reading unavailable"
        : level == BatteryAlertLevel::ShutdownRisk ? "Battery extremely low"
        : level == BatteryAlertLevel::Critical ? "Critical battery level"
        : level == BatteryAlertLevel::Low ? "Low battery warning"
        : "Battery level recovered";
    if (policy.available()) {
      doc["battery_percentage"] = reading.percent;
      String message = "Battery is at " + String(reading.percent, 1) + "%.";
      if (active) message += " Reduce non-essential loads and recharge.";
      doc["message"] = message;
    } else {
      doc["battery_percentage"] = nullptr;
      doc["message"] = "The previous battery alert is no longer current because BMS data is unavailable.";
    }
  }
  String json;
  serializeJson(doc, json);
  return json;
}

bool copySnapshot(String& data, String* battery = nullptr, String* unavailable = nullptr) {
  if (!snapshotMutex || xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  data = snapshotJson;
  if (battery) *battery = batteryJson;
  if (unavailable) *unavailable = unavailableJson;
  xSemaphoreGive(snapshotMutex);
  return true;
}
}  // namespace

void initOfflineBatteryNotifications(AsyncWebServer& server) {
  snapshotMutex = xSemaphoreCreateMutex();
  if (!snapshotMutex) {
    Serial.println("[Battery alerts] Unable to allocate snapshot mutex");
    return;
  }
  char identity[32];
  const uint64_t chip = ESP.getEfuseMac();
  snprintf(identity, sizeof(identity), "collector-%04X%08X",
           static_cast<unsigned>(chip >> 32), static_cast<unsigned>(chip));
  collectorId = identity;
  reserveBootSequence();
  policy = BatteryAlertPolicy(millis());
  serviceOfflineBatteryNotifications();

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest* request) {
    String data;
    if (!copySnapshot(data)) {
      request->send(503, "application/json", "{\"error\":\"Battery snapshot temporarily unavailable\"}");
      return;
    }
    auto* response = request->beginResponse(200, "application/json", data);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  events.onConnect([](AsyncEventSourceClient* client) {
    String data, battery, unavailable;
    client->send("{\"status\":\"connected\"}", "ready", 0, 1000);
    if (copySnapshot(data, &battery, &unavailable)) {
      client->send(data.c_str(), "state");
      // Replay inactive records as well: reconnecting apps must cancel alerts
      // that recovered while disconnected. Payload sequence drives deduping.
      client->send(battery.c_str(), "notification");
      client->send(unavailable.c_str(), "notification");
    }
  });
  server.addHandler(&events);
  Serial.println("[Battery alerts] Local /data and /events endpoints ready");
}

void serviceOfflineBatteryNotifications() {
  if (!snapshotMutex) return;
  const uint32_t now = millis();
  if (sampled && static_cast<uint32_t>(now - lastSampleAt) < 1000) return;
  sampled = true;
  lastSampleAt = now;
  JKPacketData bms = {};
  const bool valid = getBmsData(bms);
  const BatteryReading reading = {valid, static_cast<float>(bms.SOC), bms.packVoltage, bms.receivedAtMs};
  const uint8_t changes = policy.update(reading, now);
  if (changes & BatteryAlertPolicy::BatteryChanged) batterySequence = ++sequence;
  if (changes & BatteryAlertPolicy::AvailabilityChanged) unavailableSequence = ++sequence;

  const String battery = alertJson(false, reading);
  const String unavailable = alertJson(true, reading);
  JsonDocument doc;
  doc["device_id"] = collectorId;
  doc["device_state"] = deviceState ? "On" : "Off";
  auto status = doc["battery"].to<JsonObject>();
  status["valid"] = policy.available();
  status["status"] = policy.available() ? "available" : "unavailable";
  if (policy.available()) {
    status["percentage"] = reading.percent;
    status["voltage"] = reading.voltage;
    status["age_ms"] = static_cast<uint32_t>(now - reading.receivedAtMs);
  } else {
    status["percentage"] = nullptr;
    status["voltage"] = nullptr;
    if (valid) status["age_ms"] = static_cast<uint32_t>(now - reading.receivedAtMs);
    else status["age_ms"] = nullptr;
  }
  auto notifications = doc["notifications"].to<JsonArray>();
  JsonDocument parsed;
  deserializeJson(parsed, battery);
  notifications.add(parsed.as<JsonObjectConst>());
  deserializeJson(parsed, unavailable);
  notifications.add(parsed.as<JsonObjectConst>());
  String data;
  serializeJson(doc, data);

  // Requests only copy immutable snapshots; they never advance the policy.
  xSemaphoreTake(snapshotMutex, portMAX_DELAY);
  snapshotJson = data;
  batteryJson = battery;
  unavailableJson = unavailable;
  xSemaphoreGive(snapshotMutex);
  if (changes & BatteryAlertPolicy::BatteryChanged) {
    events.send(battery.c_str(), "notification");
    Serial.printf("[Battery alerts] %s\n", battery.c_str());
  }
  if (changes & BatteryAlertPolicy::AvailabilityChanged) {
    events.send(unavailable.c_str(), "notification");
    Serial.printf("[Battery alerts] %s\n", unavailable.c_str());
  }
  if (changes || static_cast<uint32_t>(now - lastHeartbeatAt) >= 10000) {
    lastHeartbeatAt = now;
    events.send(data.c_str(), "state");
    events.send("{}", "heartbeat");
  }
}
