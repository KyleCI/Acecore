// Smart-socket link over authenticated, encrypted ESP-NOW.
// Vendored from collector 1.0.4 (src/lora.cpp, Oct 14 2026) with these changes:
//  - keys come from NVS instead of a compiled header (no secrets in source)
//  - serial console handling moved to console.cpp
//  - offline events go through offlineq (bounded RAM ring, non-blocking)
//  - "lora" names replaced; the radio is ESP-NOW
#include "../prefs_safe.h"
#include "config.h"
#if FEATURE_ESPNOW_SOCKETS
#include "sockets.h"
#include "espnow_protocol.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <map>
#include "../log.h"
#include "../offline_queue.h"
#include "../settings.h"

uint8_t ESPNOW_PMK[16];
uint8_t ESPNOW_LMK[16];
uint8_t ESPNOW_AUTH_KEY[16];
static bool g_keysLoaded = false;

static inline void diagnostic(const char *level, const char *component, const char *code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static inline void diagnostic(const char *level, const char *component, const char *code, const char *fmt, ...) {
  char msg[224]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  logf(level, component, code, "%s", msg);
}
static inline bool queueOfflineEvent(const char *action, const String &id) { return offlineq::queue(action, id.c_str()); }

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t socketsMutex;
TaskHandle_t socketsTaskHandle;
namespace {
struct Received { uint8_t mac[6]; SocketLink::Packet packet; };
struct Device {
  SocketReading reading;
  uint32_t session = 0, sequence = 0, heardSession = 0;
  bool seen = false, registered = false;
  bool contacted = false;
  unsigned long lastContact = 0;
  int restartReason = -1;
  uint32_t command = 0, ack = 0;
  bool desired = false, pending = false;
  unsigned attempts = 0;
  unsigned long lastAttempt = 0;
  String commandStatus;
  bool pairing = false, forgetting = false, availableToPair = false, forgetEventStored = false;
  uint32_t lifecycleSequence = 0;
  unsigned long lifecycleAttempt = 0, pairingStarted = 0;
};
QueueHandle_t incoming;
std::map<String, Device> devices;
bool ready = false;
uint32_t session, nextCommand = 0;

String idFor(const uint8_t *mac) {
  char id[13];
  snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return id;
}
const char *restartName(int reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "sleep";
    case ESP_RST_EXT: return "reset_button";
    default: return "unknown";
  }
}
void receive(const uint8_t *mac, const uint8_t *bytes, int length) {
  if (!mac || !bytes || length != sizeof(SocketLink::Packet)) return;
  Received event;
  memcpy(event.mac, mac, 6);
  memcpy(&event.packet, bytes, sizeof(event.packet));
  xQueueSend(incoming, &event, 0);
}
bool peer(const uint8_t *mac, bool encrypt) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  memcpy(info.lmk, ESPNOW_LMK, 16);
  info.ifidx = WIFI_IF_STA;
  info.channel = 0; // Follow collector's current hotspot/router channel.
  info.encrypt = encrypt;
  return esp_now_add_peer(&info) == ESP_OK;
}
bool transmit(const uint8_t *mac, SocketLink::Packet &packet) {
  return SocketLink::authenticate(packet, false)
      && esp_now_send(mac, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)) == ESP_OK;
}
bool initialize() {
  if (WiFi.getMode() == WIFI_OFF) return false;
  WiFi.setSleep(false);
  if (esp_now_init() != ESP_OK) return false;
  if (esp_now_set_pmk(ESPNOW_PMK) != ESP_OK || esp_now_register_recv_cb(receive) != ESP_OK
      || !peer(SocketLink::Broadcast, false)) { esp_now_deinit(); return false; }
  ready = true;
  diagnostic("INFO", "ESPNOW", "READY", "collector=%s channel=%d encrypted_unicast=1", WiFi.macAddress().c_str(), WiFi.channel());
  return true;
}
}

void socketsInit() {
  g_keysLoaded = settings::espnowKeys(ESPNOW_PMK, ESPNOW_LMK, ESPNOW_AUTH_KEY);
  if (!g_keysLoaded) LOGW("ESPNOW", "NO_KEYS", "ESP-NOW keys not provisioned (SET espnow_keys <96 hex>); socket link disabled");
  dataMutex = xSemaphoreCreateMutex();
  socketsMutex = xSemaphoreCreateMutex();
  incoming = xQueueCreate(16, sizeof(Received));
  session = esp_random();
  if (!session) session = 1;
  if (!dataMutex || !socketsMutex || !incoming) ESP.restart();
}
bool socketsRadioReady() { return ready; }
void socketsDiag(const char *tag) {
  diagnostic("INFO", "ESPNOW", "STATUS", "stage=%s ready=%d channel=%d", tag, ready, WiFi.channel());
}
bool checkRegistered(String id) {
  id.replace(":", ""); id.toUpperCase();
  Preferences registry;
  if (!prefsOpenRO(registry, "registry")) return false;
  bool registered = registry.getBool(id.c_str(), false);
  registry.end();
  return registered;
}
bool registerDevice(String id) {
  id.replace(":", ""); id.toUpperCase();
  uint8_t mac[6]; if (!SocketLink::macFromId(id.c_str(), mac)) return false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto found = devices.find(id);
  bool accepted = found != devices.end() && found->second.contacted
      && millis() - found->second.lastContact < 10000 && !found->second.forgetting
      && (found->second.registered || found->second.availableToPair);
  if (accepted) {
    Device &device = found->second;
    if (!device.registered && !device.pairing) {
      device.pairing = true; device.pairingStarted = millis();
      device.lifecycleSequence = ++nextCommand; device.lifecycleAttempt = 0;
    }
  }
  xSemaphoreGive(dataMutex);
  return accepted;
}
void unregisterDevice(String id) {
  id.replace(":", ""); id.toUpperCase();
  uint8_t mac[6]; if (!SocketLink::macFromId(id.c_str(), mac)) return;
  Preferences forgotten;
  if (!forgotten.begin("forgotten", false)) return;
  bool stored = forgotten.getBool(id.c_str(), false)
      || forgotten.putBool(id.c_str(), true) == 1;
  forgotten.end();
  if (!stored) return;
  Preferences registry;
  if (registry.begin("registry", false)) {
    if (registry.isKey(id.c_str())) registry.remove(id.c_str());
    registry.end();
  }
  bool newRequest = false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Device &device = devices[id];
    device.registered = false; device.pending = false; device.pairing = false;
    if (!device.forgetting) {
      newRequest = true;
      device.forgetting = true; device.lifecycleSequence = ++nextCommand; device.lifecycleAttempt = 0;
      device.forgetEventStored = queueOfflineEvent("socket_forget_requested", id);
    }
    xSemaphoreGive(dataMutex);
  }
  if (newRequest) {
    diagnostic("INFO", "ESPNOW", "FORGET_PENDING", "socket=%s; waiting for hardware acknowledgement", id.c_str());
  }
}
bool populateDeviceArray(JsonArray &array) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  for (const auto &entry : devices) {
    const Device &device = entry.second;
    if (!device.registered && !device.pairing) continue;
    JsonObject object = array.add<JsonObject>();
    object["deviceId"] = entry.first;
    object["current"] = device.seen ? device.reading.current : 0;
    object["relayState"] = device.seen ? device.reading.relayState : 0;
    object["rssi"] = nullptr; // Receive API on this SDK does not supply RSSI.
    unsigned long age = millis() - device.reading.receivedAt;
    object["last_seen_age_ms"] = device.seen ? age : 0;
    bool online = !device.pairing && ready && device.contacted && millis() - device.lastContact < 20000;
    object["online"] = online;
    if (device.contacted) object["last_contact_age_ms"] = millis() - device.lastContact;
    else object["last_contact_age_ms"] = nullptr;
    object["connection_reason"] = !ready ? "collector_radio_unavailable"
        : online ? "connected" : "connection_lost";
    if (device.restartReason >= 0) object["last_restart_reason"] = restartName(device.restartReason);
    else object["last_restart_reason"] = nullptr;
    object["pairing_pending"] = device.pairing;
    object["transport"] = "espnow";
    object["command_sequence"] = device.command;
    object["ack_sequence"] = device.ack;
    object["command_status"] = device.commandStatus;
  }
  xSemaphoreGive(dataMutex);
  return true;
}
void populateDiscoveredSockets(JsonArray &array) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  for (const auto &entry : devices) {
    const Device &device = entry.second;
    if (device.registered || device.pairing || device.forgetting || !device.availableToPair || !device.contacted || millis() - device.lastContact >= 10000) continue;
    JsonObject object = array.add<JsonObject>();
    object["deviceId"] = entry.first;
    object["relayState"] = 0;
    object["current"] = 0;
    object["online"] = false;
    object["available_to_pair"] = true;
  }
  xSemaphoreGive(dataMutex);
}
String buildMetricsJson() {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  populateDeviceArray(array);
  String output; serializeJson(doc, output); return output;
}
bool sendResetLockout(String) { return false; }  // ESP-NOW sockets have no lockout
bool sendSetLimit(String, uint8_t) { return false; }  // nor a user limit
uint32_t socketCommandSequence(String id) {
  id.replace(":", ""); id.toUpperCase();
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
  auto found = devices.find(id);
  uint32_t sequence = found == devices.end() ? 0 : found->second.command;
  xSemaphoreGive(dataMutex); return sequence;
}
bool sendRelayCommand(String id, bool turnOn) {
  id.replace(":", ""); id.toUpperCase();
  if (!ready || xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto found = devices.find(id);
  bool accepted = false;
  if (found != devices.end()) {
    Device &device = found->second;
    if (device.registered && device.seen && millis() - device.reading.receivedAt < 20000) {
      if (!device.pending) {
        device.command = ++nextCommand; if (!device.command) device.command = ++nextCommand;
        device.desired = turnOn; device.pending = true;
        device.attempts = 0; device.lastAttempt = 0; device.commandStatus = "pending";
        accepted = true;
      } else accepted = device.desired == turnOn;
    }
  }
  xSemaphoreGive(dataMutex);
  return accepted;
}
void socketsTask(void *) {
  unsigned long nextBeacon = 0, nextRecovery = 0;
  for (;;) {
    if (!ready && millis() >= nextRecovery) { initialize(); nextRecovery = millis() + 5000; }
    if (ready && millis() >= nextBeacon) {
      SocketLink::Packet beacon; beacon.type = SocketLink::Beacon; beacon.session = session;
      transmit(SocketLink::Broadcast, beacon); nextBeacon = millis() + 1000;
    }
    Received event;
    while (xQueueReceive(incoming, &event, 0) == pdTRUE) {
      SocketLink::Packet packet;
      if (!SocketLink::decode(reinterpret_cast<uint8_t *>(&event.packet), sizeof(event.packet), packet)) continue;
      if (packet.type != SocketLink::Discover && packet.type != SocketLink::Telemetry
          && packet.type != SocketLink::PairAck && packet.type != SocketLink::ForgetAck) continue;
      if (!peer(event.mac, true)) continue;
      String id = idFor(event.mac);
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
      devices[id].contacted = true;
      devices[id].lastContact = millis();
      devices[id].heardSession = packet.session;
      xSemaphoreGive(dataMutex);
      bool registered = checkRegistered(id);
      Preferences forgotten; bool blocked = false;
      if (prefsOpenRO(forgotten, "forgotten")) { blocked = forgotten.getBool(id.c_str(), false); forgotten.end(); }
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
      Device &link = devices[id];
      if (packet.type == SocketLink::Discover && (packet.flags & SocketLink::PairingSupported)) {
        link.availableToPair = !(packet.flags & SocketLink::AlreadyPaired);
        if (link.availableToPair && registered) {
          Preferences registry;
          if (registry.begin("registry", false)) { registry.remove(id.c_str()); registry.end(); }
          registered = false; link.registered = false; link.pending = false;
        }
      }
      if (blocked && !link.forgetting) {
        link.forgetting = true; link.lifecycleSequence = ++nextCommand; link.lifecycleAttempt = 0;
      }
      if (packet.type == SocketLink::ForgetAck && link.forgetting &&
          packet.ackSession == session && packet.ackSequence == link.lifecycleSequence) {
        // Persist the cloud event before retiring the durable hardware request.
        if (!queueOfflineEvent("socket_forgotten", id)) { xSemaphoreGive(dataMutex); continue; }
        if (forgotten.begin("forgotten", false)) { forgotten.remove(id.c_str()); forgotten.end(); }
        link.forgetting = false; link.registered = false; link.seen = false;
        diagnostic("INFO", "ESPNOW", "FORGOTTEN", "socket=%s acknowledged", id.c_str());
        xSemaphoreGive(dataMutex); continue;
      }
      if (packet.type == SocketLink::PairAck && link.pairing &&
          packet.ackSession == session && packet.ackSequence == link.lifecycleSequence) {
        Preferences registry;
        if (registry.begin("registry", false)) {
          link.registered = registry.putBool(id.c_str(), true) == 1; registry.end();
          if (link.registered && queueOfflineEvent("socket_paired", id)) link.pairing = false;
        }
        xSemaphoreGive(dataMutex); continue;
      }
      xSemaphoreGive(dataMutex);
      if (packet.type != SocketLink::Telemetry || !registered || blocked) continue;
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
      Device &device = devices[id];
      if (device.seen && device.session == packet.session && !SocketLink::newer(packet.sequence, device.sequence)) {
        xSemaphoreGive(dataMutex); continue;
      }
      device.restartReason = SocketLink::restartReason(packet.flags);
      device.session = packet.session; device.sequence = packet.sequence; device.seen = true; device.registered = true;
      device.reading.deviceId = id; device.reading.current = packet.current;
      device.reading.relayState = packet.relay; device.reading.receivedAt = millis();
      diagnostic("INFO", "ESPNOW", "TELEMETRY", "socket=%s current=%.3fA relay=%s seq=%lu", id.c_str(), packet.current, packet.relay ? "ON" : "OFF", (unsigned long)packet.sequence);
      if (device.pending && packet.ackSession == session && packet.ackSequence == device.command) {
        device.ack = packet.ackSequence; device.pending = false;
        device.commandStatus = packet.relay == device.desired && !(packet.flags & 1) ? "confirmed" : "rejected";
        diagnostic("INFO", "ESPNOW", "COMMAND_ACK", "socket=%s seq=%lu desired=%s actual=%s status=%s", id.c_str(), (unsigned long)device.command, device.desired ? "ON" : "OFF", packet.relay ? "ON" : "OFF", device.commandStatus.c_str());
      }
      xSemaphoreGive(dataMutex);
    }
    if (ready && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      for (auto &entry : devices) {
        Device &device = entry.second;
        if (device.pairing && !device.registered && millis() - device.pairingStarted > 10000) device.pairing = false;
        if ((device.pairing || device.forgetting) &&
            (!device.lifecycleAttempt || millis() - device.lifecycleAttempt >= 1000)) {
          if (device.forgetting && !device.forgetEventStored)
            device.forgetEventStored = queueOfflineEvent("socket_forget_requested", entry.first);
          uint8_t mac[6];
          if (SocketLink::macFromId(entry.first.c_str(), mac)) {
            SocketLink::Packet message; message.type = device.forgetting ? SocketLink::Forget : SocketLink::Pair;
            message.session = session; message.sequence = device.lifecycleSequence;
            message.ackSession = device.heardSession;
            if (peer(mac, true)) transmit(mac, message);
            device.lifecycleAttempt = millis();
          }
        }
        if (!device.pending || (device.attempts && millis() - device.lastAttempt < 750)) continue;
        if (device.attempts >= 6) {
          device.pending = false; device.commandStatus = "timeout";
          diagnostic("ERROR", "ESPNOW", "COMMAND_TIMEOUT", "socket=%s seq=%lu", entry.first.c_str(), (unsigned long)device.command);
          continue;
        }
        uint8_t mac[6];
        if (!SocketLink::macFromId(entry.first.c_str(), mac)) continue;
        SocketLink::Packet command; command.type = SocketLink::Command;
        command.session = session; command.sequence = device.command; command.relay = device.desired;
        bool sent = peer(mac, true) && transmit(mac, command);
        device.lastAttempt = millis(); ++device.attempts;
        diagnostic(sent ? "INFO" : "WARN", "ESPNOW", "COMMAND_TX", "socket=%s seq=%lu state=%s attempt=%u queued=%d", entry.first.c_str(), (unsigned long)device.command, device.desired ? "ON" : "OFF", device.attempts, sent);
      }
      xSemaphoreGive(dataMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
void socketsStart() {
  if (!g_keysLoaded) return;
  xTaskCreatePinnedToCore(socketsTask, "socket_link", 6144, nullptr, 2, &socketsTaskHandle, 1);
}

const char *socketTransport() { return "espnow"; }
void socketsSetLowPower(bool) {}
#endif  // FEATURE_ESPNOW_SOCKETS
