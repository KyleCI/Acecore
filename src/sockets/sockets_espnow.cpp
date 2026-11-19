// Smart-socket link over authenticated, encrypted ESP-NOW (2.5.40, Caleb 19 Nov 2026 14:31 WAT:
// ESP-NOW on, LoRa off, on all six images).
// Protocol and state machine: collector 1.0.4 (Caleb's reference "Col PL.zip", src/lora.cpp +
// include/espnow_protocol.h, close-range validated 4 Oct 2026 against socket 1.0.1). Wire format is
// byte-identical (pc_core/espnow_wire.h, 44-byte v1 packet, HMAC-SHA256/16 + PMK/LMK unicast).
// Changes from the reference:
//  - keys come from NVS (SET espnow_keys <96 hex>), never from a compiled header
//  - same app/cloud API as the LoRa path (sockets.h): cmd_id, desired/pending, supersede,
//    confirmed-from-telemetry, change push (takeSocketsChanged), socket power VA, alert keys
//  - encrypted-peer budget (ESP-IDF max 7): paired / pairing sockets are never evicted, idle
//    unpaired observations are; the device map is bounded (32)
//  - paused during a local OTA upload; Wi-Fi power save off while the link runs (modem sleep
//    drops ESP-NOW frames), modem sleep only in the battery low-power latch
//  - offline events through offlineq; serial console handling lives in console.cpp
// Channel: peers use channel 0 = the collector's current channel (router channel when the station
// is up, hotspot channel otherwise; AP and STA share one channel). The socket scans 1..13 for the
// collector's beacon, so a router channel change is followed without pinning.
#include "../prefs_safe.h"
#include "config.h"
#if FEATURE_ESPNOW_SOCKETS
#include "sockets.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <map>
#include <vector>
#include "../log.h"
#include "../offline_queue.h"
#include "../settings.h"
#include "espnow_wire.h"

namespace W = pc::espnow;

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t socketsMutex;
TaskHandle_t socketsTaskHandle;

static volatile bool g_socketsChanged = false;
static volatile bool g_paused = false;    // local OTA upload (POST /ota)
static volatile bool g_lowPower = false;  // battery critical latch
namespace {
uint8_t g_pmk[16], g_lmk[16], g_auth[16];
bool g_keysLoaded = false;
constexpr uint32_t kOnlineMs = 20000;     // reference: online if heard in the last 20 s
constexpr uint32_t kCommandMs = 60000;    // same as LoRa 2.5.11: a socket heard in 60 s takes a command
constexpr uint32_t kPairWindowMs = 10000;
constexpr uint32_t kRetryMs = 750;        // reference command retry
constexpr unsigned kMaxAttempts = 6;
constexpr uint32_t kBeaconMs = 1000;      // socket rescans after 3.5 s without a beacon
constexpr size_t kMaxDevices = 32;

struct Received { uint8_t mac[6]; uint8_t bytes[sizeof(W::Packet)]; };
struct Device {
  SocketReading reading;
  uint32_t session = 0, sequence = 0, heardSession = 0;
  bool seen = false, registered = false, contacted = false, peer = false;
  unsigned long lastContact = 0;
  int restartReason = -1;
  bool overcurrent = false, rejectedFlag = false;
  uint32_t command = 0, ack = 0;
  bool desired = false, pending = false;
  char cmdId[24] = {};
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
uint32_t session = 1, nextCommand = 0;

String norm(String id) { id.replace(":", ""); id.toUpperCase(); return id; }
String idFor(const uint8_t *mac) { char id[13]; W::idFromMac(mac, id); return id; }
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
bool registryHas(const String &id) {
  Preferences r;
  if (!prefsOpenRO(r, "registry")) return false;
  bool v = r.getBool(id.c_str(), false);
  r.end();
  return v;
}
bool forgottenHas(const String &id) {
  Preferences f;
  if (!prefsOpenRO(f, "forgotten")) return false;
  bool v = f.getBool(id.c_str(), false);
  f.end();
  return v;
}
// Wi-Fi callback: copy and enqueue only.
void receive(const uint8_t *mac, const uint8_t *bytes, int length) {
  if (!mac || !bytes || length != (int)sizeof(W::Packet)) return;
  Received e;
  memcpy(e.mac, mac, 6);
  memcpy(e.bytes, bytes, sizeof e.bytes);
  xQueueSend(incoming, &e, 0);
}
bool addPeer(const uint8_t *mac, bool encrypt) {
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  memcpy(info.lmk, g_lmk, 16);
  info.ifidx = WIFI_IF_STA;
  info.channel = 0;  // follow the collector's current router / hotspot channel
  info.encrypt = encrypt;
  return esp_now_add_peer(&info) == ESP_OK;
}
// dataMutex held. Encrypted peer for this socket, evicting the stalest unpaired peer if full.
bool ensurePeer(const String &id, Device &dev) {
  uint8_t mac[6];
  if (!W::macFromId(id.c_str(), mac)) return false;
  if (esp_now_is_peer_exist(mac)) { dev.peer = true; return true; }
  std::vector<W::PeerEntry> entries;
  std::vector<String> ids;
  for (auto &kv : devices) {
    if (!kv.second.peer) continue;
    W::PeerEntry p;
    p.present = true; p.registered = kv.second.registered;
    p.lifecycle = kv.second.pairing || kv.second.forgetting;
    p.lastContact = kv.second.lastContact;
    entries.push_back(p); ids.push_back(kv.first);
  }
  int victim = W::pickPeerEviction(entries.data(), entries.size(), millis());
  if (victim == -2) { LOGW("ESPNOW", "PEER_FULL", "socket=%s not added: %u encrypted peers in use", id.c_str(), W::kMaxEncryptedPeers); return false; }
  if (victim >= 0) {
    uint8_t vm[6];
    if (W::macFromId(ids[victim].c_str(), vm)) esp_now_del_peer(vm);
    devices[ids[victim]].peer = false;
  }
  dev.peer = addPeer(mac, true);
  return dev.peer;
}
bool transmit(const uint8_t *mac, W::Packet &p) {
  W::sign(p, g_auth);
  return esp_now_send(mac, reinterpret_cast<const uint8_t *>(&p), sizeof p) == ESP_OK;
}
bool initialize() {
  if (WiFi.getMode() == WIFI_OFF) return false;
  if (!g_lowPower) WiFi.setSleep(false);  // reference: WIFI_PS_NONE, modem sleep misses frames
  if (esp_now_init() != ESP_OK) return false;
  if (esp_now_set_pmk(g_pmk) != ESP_OK || esp_now_register_recv_cb(receive) != ESP_OK || !addPeer(W::Broadcast, false)) {
    esp_now_deinit();
    return false;
  }
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (auto &kv : devices) kv.second.peer = false;
    xSemaphoreGive(dataMutex);
  }
  ready = true;
  LOGI("ESPNOW", "READY", "collector=%s channel=%d encrypted_unicast=1", WiFi.macAddress().c_str(), WiFi.channel());
  return true;
}
// dataMutex held. Bounded map: drop the stalest unregistered, idle observation.
Device *deviceFor(const String &id, bool registered) {
  auto f = devices.find(id);
  if (f != devices.end()) return &f->second;
  if (devices.size() >= kMaxDevices) {
    auto victim = devices.end();
    for (auto it = devices.begin(); it != devices.end(); ++it) {
      const Device &d = it->second;
      if (d.registered || d.pairing || d.forgetting) continue;
      if (victim == devices.end() || d.lastContact < victim->second.lastContact) victim = it;
    }
    if (victim == devices.end() && !registered) return nullptr;
    if (victim != devices.end()) {
      uint8_t vm[6];
      if (victim->second.peer && W::macFromId(victim->first.c_str(), vm)) esp_now_del_peer(vm);
      devices.erase(victim);
    }
  }
  return &devices[id];
}

void handle(const Received &e) {
  W::Packet p;
  if (!W::decode(e.bytes, sizeof e.bytes, p, g_auth)) return;
  if (p.type != W::Discover && p.type != W::Telemetry && p.type != W::PairAck && p.type != W::ForgetAck) return;
  String id = idFor(e.mac);
  bool registered = registryHas(id);
  bool blocked = forgottenHas(id);
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  Device *dp = deviceFor(id, registered || blocked);
  if (!dp) { xSemaphoreGive(dataMutex); return; }
  Device &d = *dp;
  d.contacted = true; d.lastContact = millis(); d.heardSession = p.session;
  d.registered = registered && !blocked;
  ensurePeer(id, d);
  if (p.type == W::Discover && (p.flags & W::PairingSupported)) {
    d.availableToPair = !(p.flags & W::AlreadyPaired);
    if (d.availableToPair && registered) {
      // The socket says it is no longer paired (forgotten or reset there): drop our registry entry.
      Preferences r;
      if (r.begin("registry", false)) { r.remove(id.c_str()); r.end(); }
      registered = false; d.registered = false; d.pending = false; g_socketsChanged = true;
      LOGW("ESPNOW", "UNPAIRED_BY_SOCKET", "socket=%s reports not paired; registry entry removed", id.c_str());
    }
  }
  if (blocked && !d.forgetting) {
    d.forgetting = true; d.lifecycleSequence = ++nextCommand; d.lifecycleAttempt = 0;
  }
  if (p.type == W::ForgetAck && d.forgetting && p.ackSession == session && p.ackSequence == d.lifecycleSequence) {
    // Persist the cloud event before retiring the durable hardware request.
    if (offlineq::queue("socket_forgotten", id.c_str())) {
      Preferences f;
      if (f.begin("forgotten", false)) { f.remove(id.c_str()); f.end(); }
      d.forgetting = false; d.registered = false; d.seen = false; g_socketsChanged = true;
      LOGI("ESPNOW", "FORGOTTEN", "socket=%s acknowledged", id.c_str());
    }
    xSemaphoreGive(dataMutex); return;
  }
  if (p.type == W::PairAck && d.pairing && p.ackSession == session && p.ackSequence == d.lifecycleSequence) {
    Preferences r;
    if (r.begin("registry", false)) {
      d.registered = r.putBool(id.c_str(), true) == 1; r.end();
      if (d.registered && offlineq::queue("socket_paired", id.c_str())) d.pairing = false;
      if (d.registered) { g_socketsChanged = true; LOGI("ESPNOW", "PAIRED", "socket=%s", id.c_str()); }
    }
    xSemaphoreGive(dataMutex); return;
  }
  if (p.type != W::Telemetry || !d.registered) { xSemaphoreGive(dataMutex); return; }
  if (d.seen && d.session == p.session && !W::newer(p.sequence, d.sequence)) { xSemaphoreGive(dataMutex); return; }
  if (!d.seen || d.reading.relayState != p.relay) g_socketsChanged = true;
  bool ocEdge = bool(p.flags & W::TelemetryOvercurrent) != d.overcurrent;
  d.restartReason = W::restartReason(p.flags);
  d.overcurrent = p.flags & W::TelemetryOvercurrent;
  d.rejectedFlag = p.flags & W::TelemetryRejected;
  d.session = p.session; d.sequence = p.sequence; d.seen = true;
  d.reading.deviceId = id; d.reading.current = p.current; d.reading.relayState = p.relay;
  d.reading.rssi = 0; d.reading.receivedAt = millis();
  bool stateMatch = d.command && p.relay == (d.desired ? 1 : 0);
  if (d.pending && p.ackSession == session && p.ackSequence == d.command) {
    d.ack = p.ackSequence; d.pending = false; g_socketsChanged = true;
    d.commandStatus = stateMatch && !d.rejectedFlag ? "confirmed" : "rejected";
    LOGI("ESPNOW", "COMMAND_ACK", "socket=%s id=%s desired=%s actual=%s status=%s", id.c_str(), d.cmdId,
         d.desired ? "ON" : "OFF", p.relay ? "ON" : "OFF", d.commandStatus.c_str());
  } else if (stateMatch && (d.pending || d.commandStatus != "confirmed") && !d.rejectedFlag) {
    // Same rule as LoRa 2.5.11: the ack was missed but the socket reports the state we asked for.
    bool wasPending = d.pending;
    d.pending = false; d.ack = d.command; d.commandStatus = "confirmed"; g_socketsChanged = true;
    LOGI("ESPNOW", "COMMAND_STATE_OK", "socket=%s id=%s relay=%s %s", id.c_str(), d.cmdId, p.relay ? "ON" : "OFF",
         wasPending ? "(ack not matched)" : "(after timeout)");
  }
  float amps = p.current;
  xSemaphoreGive(dataMutex);
  if (ocEdge) LOGW("ESPNOW", "SOCKET_ALERT", "socket=%s current=%.2fA overcurrent=%d", id.c_str(), amps, (int)bool(p.flags & W::TelemetryOvercurrent));
}

void service() {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  unsigned long now = millis();
  for (auto &kv : devices) {
    Device &d = kv.second;
    if (d.pairing && !d.registered && now - d.pairingStarted > kPairWindowMs) { d.pairing = false; g_socketsChanged = true; }
    uint8_t mac[6];
    if (!W::macFromId(kv.first.c_str(), mac)) continue;
    if ((d.pairing || d.forgetting) && (!d.lifecycleAttempt || now - d.lifecycleAttempt >= 1000)) {
      if (d.forgetting && !d.forgetEventStored) d.forgetEventStored = offlineq::queue("socket_forget_requested", kv.first.c_str());
      W::Packet m;
      m.type = d.forgetting ? W::Forget : W::Pair;
      m.session = session; m.sequence = d.lifecycleSequence; m.ackSession = d.heardSession;
      if (ensurePeer(kv.first, d)) transmit(mac, m);
      d.lifecycleAttempt = now;
    }
    if (!d.pending || (d.attempts && now - d.lastAttempt < kRetryMs)) continue;
    if (d.attempts >= kMaxAttempts) {
      d.pending = false; d.commandStatus = "timeout"; g_socketsChanged = true;
      LOGE("ESPNOW", "COMMAND_TIMEOUT", "socket=%s id=%s", kv.first.c_str(), d.cmdId);
      continue;
    }
    W::Packet c;
    c.type = W::Command; c.session = session; c.sequence = d.command; c.relay = d.desired;
    bool sent = ensurePeer(kv.first, d) && transmit(mac, c);
    d.lastAttempt = now; ++d.attempts;
    LOGI("ESPNOW", "COMMAND_TX", "socket=%s id=%s state=%s attempt=%u queued=%d", kv.first.c_str(), d.cmdId,
         d.desired ? "ON" : "OFF", d.attempts, sent);
  }
  xSemaphoreGive(dataMutex);
}

void socketsTask(void *) {
  unsigned long nextBeacon = 0, nextRecovery = 0;
  for (;;) {
    if (g_paused) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    if (ready && WiFi.getMode() == WIFI_OFF) {  // Wi-Fi stopped under us: re-init when it is back
      esp_now_deinit(); ready = false;
      LOGW("ESPNOW", "WIFI_OFF", "radio stopped; ESP-NOW will re-init");
    }
    if (!ready && millis() >= nextRecovery) { initialize(); nextRecovery = millis() + 5000; }
    if (ready && millis() >= nextBeacon) {
      W::Packet b; b.type = W::Beacon; b.session = session;
      transmit(W::Broadcast, b);
      nextBeacon = millis() + kBeaconMs;
    }
    Received e;
    while (xQueueReceive(incoming, &e, 0) == pdTRUE) handle(e);
    if (ready) service();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool queueCommand(String id, bool turnOn) {
  id = norm(id);
  if (!ready || xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto f = devices.find(id);
  bool accepted = false;
  if (f != devices.end()) {
    Device &d = f->second;
    bool supersede = d.pending && d.desired != turnOn;  // a new tap for the other state wins
    if (d.registered && d.seen && millis() - d.reading.receivedAt < kCommandMs) {
      if (!d.pending || supersede) {
        d.command = ++nextCommand; if (!d.command) d.command = ++nextCommand;
        snprintf(d.cmdId, sizeof d.cmdId, "e%06lx-%lu", (unsigned long)(session & 0xFFFFFF), (unsigned long)d.command);
        d.desired = turnOn; d.pending = true; d.attempts = 0; d.lastAttempt = 0; d.commandStatus = "pending";
        accepted = true; g_socketsChanged = true;
      } else accepted = d.desired == turnOn;
    }
  }
  xSemaphoreGive(dataMutex);
  return accepted;
}
}  // namespace

void socketsInit() {
  g_keysLoaded = settings::espnowKeys(g_pmk, g_lmk, g_auth);
  if (!g_keysLoaded) LOGW("ESPNOW", "NO_KEYS", "ESP-NOW keys not provisioned (SET espnow_keys <96 hex>); smart sockets off");
  dataMutex = xSemaphoreCreateMutex();
  socketsMutex = xSemaphoreCreateMutex();
  incoming = xQueueCreate(16, sizeof(Received));
  session = esp_random();
  if (!session) session = 1;
  if (!dataMutex || !socketsMutex || !incoming) ESP.restart();
}
void socketsStart() {
  if (!g_keysLoaded) { LOGE("ESPNOW", "SOCKETS_OFF", "no espnow_keys in NVS; smart sockets unavailable"); return; }
  xTaskCreatePinnedToCore(socketsTask, "socket_link", 6144, nullptr, 2, &socketsTaskHandle, 1);
}
bool socketsRadioReady() { return ready; }
const char *socketTransport() { return "espnow"; }
void socketsDiag(const char *tag) {
  LOGI("ESPNOW", "STATUS", "stage=%s ready=%d keys=%d channel=%d devices=%u", tag, ready, g_keysLoaded, WiFi.channel(), (unsigned)devices.size());
}
bool checkRegistered(String id) { return registryHas(norm(id)); }
bool registerDevice(String id) {
  id = norm(id);
  uint8_t mac[6];
  if (!W::macFromId(id.c_str(), mac)) return false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto f = devices.find(id);
  // Reference rule: heard in the last 10 s, not being forgotten, and either already paired or
  // advertising "available to pair". The socket then confirms with PairAck before the registry write.
  bool accepted = f != devices.end() && f->second.contacted && millis() - f->second.lastContact < kPairWindowMs &&
                  !f->second.forgetting && (f->second.registered || f->second.availableToPair);
  if (accepted) {
    Device &d = f->second;
    if (!d.registered && !d.pairing) {
      d.pairing = true; d.pairingStarted = millis();
      d.lifecycleSequence = ++nextCommand; d.lifecycleAttempt = 0;
      g_socketsChanged = true;
    }
  }
  xSemaphoreGive(dataMutex);
  return accepted;
}
void unregisterDevice(String id) {
  id = norm(id);
  uint8_t mac[6];
  if (!W::macFromId(id.c_str(), mac)) return;
  Preferences f;
  if (!f.begin("forgotten", false)) return;
  bool stored = f.getBool(id.c_str(), false) || f.putBool(id.c_str(), true) == 1;
  f.end();
  if (!stored) return;
  Preferences r;
  if (r.begin("registry", false)) { if (r.isKey(id.c_str())) r.remove(id.c_str()); r.end(); }
  bool newRequest = false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Device *dp = deviceFor(id, true);
    if (dp) {
      Device &d = *dp;
      d.registered = false; d.pending = false; d.pairing = false;
      if (!d.forgetting) {
        newRequest = true;
        d.forgetting = true; d.lifecycleSequence = ++nextCommand; d.lifecycleAttempt = 0;
        d.forgetEventStored = offlineq::queue("socket_forget_requested", id.c_str());
      }
      g_socketsChanged = true;
    }
    xSemaphoreGive(dataMutex);
  }
  if (newRequest) LOGI("ESPNOW", "FORGET_PENDING", "socket=%s; hidden now, waiting for the socket to acknowledge", id.c_str());
}
bool populateDeviceArray(JsonArray &array) {
  const SocketVoltage volts = socketsCollectorVoltage();  // before dataMutex (own locks)
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  unsigned long now = millis();
  for (const auto &kv : devices) {
    const Device &d = kv.second;
    if (!d.registered && !d.pairing) continue;
    JsonObject o = array.add<JsonObject>();
    unsigned long age = now - d.reading.receivedAt;
    bool online = !d.pairing && ready && d.contacted && now - d.lastContact < kOnlineMs;
    o["deviceId"] = kv.first;
    o["current"] = d.seen ? d.reading.current : 0;
    o["relayState"] = d.seen ? d.reading.relayState : 0;
    o["rssi"] = nullptr;  // this SDK's ESP-NOW receive callback has no RSSI
    o["snr"] = nullptr;
    o["last_seen_age_ms"] = d.seen ? age : 0;
    o["online"] = online;
    if (d.contacted) o["last_contact_age_ms"] = now - d.lastContact; else o["last_contact_age_ms"] = nullptr;
    o["connection_reason"] = !ready ? "collector_radio_unavailable" : online ? "connected" : "connection_lost";
    if (d.restartReason >= 0) o["last_restart_reason"] = restartName(d.restartReason); else o["last_restart_reason"] = nullptr;
    o["pairing_pending"] = d.pairing;
    o["transport"] = "espnow";
    o["firmware_version"] = nullptr;  // socket 1.0.1 does not report it over ESP-NOW
    o["command_sequence"] = d.command;
    o["ack_sequence"] = d.ack;
    o["command_status"] = d.commandStatus;
    o["desired"] = d.command ? (d.desired ? 1 : 0) : (d.seen ? d.reading.relayState : 0);
    o["pending"] = d.pending;
    if (d.command) o["cmd_id"] = d.cmdId; else o["cmd_id"] = nullptr;
    o["lockout"] = false;  // no lockout / user limit over ESP-NOW v1
    o["lockout_trip_current"] = nullptr;
    o["command_type"] = "relay";
    o["limit_confirmed_amps"] = nullptr;
    if (d.overcurrent) o["alert"] = "overcurrent"; else o["alert"] = nullptr;
    socketPowerToJson(o, online, d.reading.current, volts);  // VA = collector V x socket I, 0.2 A floor
  }
  xSemaphoreGive(dataMutex);
  return true;
}
void populateDiscoveredSockets(JsonArray &array) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  unsigned long now = millis();
  for (const auto &kv : devices) {
    const Device &d = kv.second;
    if (d.registered || d.pairing || d.forgetting || !d.availableToPair || !d.contacted || now - d.lastContact >= kPairWindowMs) continue;
    JsonObject o = array.add<JsonObject>();
    o["deviceId"] = kv.first;
    o["relayState"] = 0;
    o["current"] = 0;
    o["online"] = false;
    o["available_to_pair"] = true;
    o["transport"] = "espnow";
  }
  xSemaphoreGive(dataMutex);
}
String buildMetricsJson() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  populateDeviceArray(a);
  String out; serializeJson(doc, out); return out;
}
uint32_t socketCommandSequence(String id) {
  id = norm(id);
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
  auto f = devices.find(id);
  uint32_t s = f == devices.end() ? 0 : f->second.command;
  xSemaphoreGive(dataMutex);
  return s;
}
bool sendRelayCommand(String id, bool turnOn) { return queueCommand(id, turnOn); }
// Socket firmware 1.0.1 has no lockout or user limit over ESP-NOW: report unsupported.
bool sendResetLockout(String) { return false; }
bool sendSetLimit(String, uint8_t) { return false; }
bool takeSocketsChanged() { bool c = g_socketsChanged; g_socketsChanged = false; return c; }
bool socketCommandInfo(String id, bool &desired, bool &pending, String &cmdId) {
  id = norm(id);
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto f = devices.find(id);
  bool found = f != devices.end() && f->second.command;
  if (found) { desired = f->second.desired; pending = f->second.pending; cmdId = f->second.cmdId; }
  xSemaphoreGive(dataMutex);
  return found;
}
void socketsSetPaused(bool on) {
  if (g_paused != on) LOGI("ESPNOW", on ? "PAUSED" : "RESUMED", "local OTA %s", on ? "upload" : "done");
  g_paused = on;
}
// Battery low-power latch: power_guard switches Wi-Fi to modem sleep. ESP-NOW keeps beaconing; frames
// may be missed while asleep, so commands still retry. Back to WIFI_PS_NONE when the latch clears.
void socketsSetLowPower(bool on) { g_lowPower = on; }
#endif  // FEATURE_ESPNOW_SOCKETS
