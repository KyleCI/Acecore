// Smart-socket link over LoRa (SX1278, 433 MHz, sync 0xF1, SF9/BW125k/CR4-5,
// preamble 12, CRC on, DIO0 polled). Port of the Dubem collector src/lora.cpp
// protocol (wire format in pc_core/lora_socket_proto.h). Caleb, Oct 22 2026:
// sockets use LoRa for range; this replaces the ESP-NOW path (kept behind
// FEATURE_ESPNOW_SOCKETS, off by default).
// Differences from the Dubem collector:
//  - relay commands are tracked: pending -> confirmed | rejected | timeout,
//    with retries (same command id, so the socket de-duplicates them)
//  - socket alerts (ALERT:OVERCURRENT, WARN:CURRENT_HIGH) feed the 2.2 event engine
//  - pairing/forget write the same NVS "registry" namespace and queue
//    socket_paired / socket_forgotten offline events
//  - the radio is shared under socketsMutex; DIO0 is never IO2 (STM32 TX)
#include "../prefs_safe.h"
#include "config.h"
#if FEATURE_LORA_SOCKETS
#include "sockets.h"
#include <LoRa.h>
#include <Preferences.h>
#include <map>
#include "../log.h"
#include "../offline_queue.h"
#include "../periph.h"
#include "lora_socket_proto.h"

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t socketsMutex;  // = periph::spiMutex(): one lock for LoRa and SD on the shared bus
TaskHandle_t socketsTaskHandle;

namespace {
constexpr uint32_t kOnlineMs = 20000, kDiscoverMs = 120000, kRetryMs = 1500;
constexpr unsigned kMaxAttempts = 4;
constexpr size_t kMaxDevices = 32;
struct Device {
  SocketReading reading;
  String fw;
  float snr = 0;
  bool seen = false, registered = false;
  bool alertOc = false, alertWarn = false;
  bool lockout = false; float lockoutA = 0;  // ALERT:LOCKOUT, last trip current (kept after reset)
  bool resetCmd = false;                     // pending command is RESET_LOCKOUT
  bool limitCmd = false; uint8_t cmdLimit = 0;  // pending command is SET_LIMIT
  uint8_t limitDa = 0;                       // LIM reported by the socket (0 = never)
  uint32_t command = 0, ack = 0;
  bool desired = false, pending = false;
  char cmdId[24] = {};
  unsigned attempts = 0;
  unsigned long lastAttempt = 0;
  String commandStatus;
  uint32_t packets = 0;
};
std::map<String, Device> devices;
bool ready = false;
uint32_t session = 0, nextCommand = 0;

String norm(String id) { id.replace(":", ""); id.toUpperCase(); return id; }
bool validId(const String &id) {
  if (id.length() != 12) return false;
  for (char c : id) if (!isxdigit((unsigned char)c)) return false;
  return true;
}
bool registryHas(const String &id) {
  Preferences r;
  if (!prefsOpenRO(r, "registry")) return false;
  bool v = r.isKey(id.c_str());
  r.end();
  return v;
}

bool transmit(const char *payload, size_t n) {
  if (xSemaphoreTake(socketsMutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  bool ok = LoRa.beginPacket() == 1;
  if (ok) { LoRa.write(reinterpret_cast<const uint8_t *>(payload), n); ok = LoRa.endPacket() == 1; }
  LoRa.idle();  // standby, so the next parsePacket() re-arms single RX cleanly
  xSemaphoreGive(socketsMutex);
  return ok;
}

void handle(const pc::lora::SocketPacket &p, int rssi, float snr) {
  String id = p.id;
  bool registered = registryHas(id);
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  if (!devices.count(id)) {
    if (devices.size() >= kMaxDevices) {  // drop the oldest unregistered observation
      auto victim = devices.end();
      for (auto it = devices.begin(); it != devices.end(); ++it)
        if (!it->second.registered && (victim == devices.end() || it->second.reading.receivedAt < victim->second.reading.receivedAt)) victim = it;
      if (victim == devices.end() && !registered) { xSemaphoreGive(dataMutex); return; }
      if (victim != devices.end()) devices.erase(victim);
    }
  }
  Device &d = devices[id];
  d.registered = registered;
  d.seen = true; ++d.packets;
  d.reading.deviceId = id; d.reading.current = p.current; d.reading.relayState = p.relay;
  d.reading.rssi = rssi; d.reading.receivedAt = millis(); d.snr = snr;
  if (p.fw[0]) d.fw = p.fw;
  bool ocEdge = p.overcurrent != d.alertOc, warnEdge = p.warning != d.alertWarn;
  d.alertOc = p.overcurrent; d.alertWarn = p.warning;
  bool lockEdge = p.lockout != d.lockout;
  d.lockout = p.lockout;
  if (p.hasTripCurrent) d.lockoutA = p.tripCurrent;
  if (p.hasLimit) d.limitDa = p.limitDa;
  if (d.pending && p.ack[0] && !strcmp(p.ack, d.cmdId)) {
    d.pending = false; d.ack = d.command;
    // The socket acknowledges a refused RELAY_ON (overcurrent latch, OTA) with relay still 0.
    // RESET_LOCKOUT is confirmed when the ack packet no longer carries ALERT:LOCKOUT.
    d.commandStatus = d.limitCmd ? (p.hasLimit && p.limitDa == d.cmdLimit ? "confirmed" : "rejected")
                    : d.resetCmd ? (!p.lockout ? "confirmed" : "rejected")
                                 : (p.relay == (d.desired ? 1 : 0)) ? "confirmed" : "rejected";
    LOGI("LORA", "COMMAND_ACK", "socket=%s id=%s desired=%s actual=%s status=%s", id.c_str(), d.cmdId,
         d.desired ? "ON" : "OFF", p.relay ? "ON" : "OFF", d.commandStatus.c_str());
  }
  xSemaphoreGive(dataMutex);
  if (lockEdge)
    LOGW("LORA", "SOCKET_LOCKOUT", "socket=%s lockout=%d last_trip=%.2fA", id.c_str(), p.lockout, p.tripCurrent);
  if (ocEdge || warnEdge)
    LOGW("LORA", "SOCKET_ALERT", "socket=%s current=%.2fA overcurrent=%d warning=%d", id.c_str(), p.current, p.overcurrent, p.warning);
}

void serviceCommands() {
  struct Tx { char buf[80]; size_t n; String id; unsigned attempt; };
  Tx tx[4]; size_t ntx = 0;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  unsigned long now = millis();
  for (auto &e : devices) {
    Device &d = e.second;
    if (!d.pending || (d.attempts && now - d.lastAttempt < kRetryMs + (esp_random() % 400))) continue;
    if (d.attempts >= kMaxAttempts) {
      d.pending = false; d.commandStatus = "timeout";
      LOGE("LORA", "COMMAND_TIMEOUT", "socket=%s id=%s", e.first.c_str(), d.cmdId);
      continue;
    }
    if (ntx == 4) continue;
    size_t n = d.limitCmd ? pc::lora::buildSetLimitCommand(e.first.c_str(), d.cmdLimit, d.cmdId, tx[ntx].buf, sizeof tx[ntx].buf)
             : d.resetCmd ? pc::lora::buildResetLockoutCommand(e.first.c_str(), d.cmdId, tx[ntx].buf, sizeof tx[ntx].buf)
                          : pc::lora::buildRelayCommand(e.first.c_str(), d.desired, d.cmdId, tx[ntx].buf, sizeof tx[ntx].buf);
    d.lastAttempt = now; ++d.attempts;
    if (n) { tx[ntx].n = n; tx[ntx].id = e.first; tx[ntx].attempt = d.attempts; ++ntx; }
  }
  xSemaphoreGive(dataMutex);
  for (size_t i = 0; i < ntx; ++i) {  // airtime (~150 ms at SF9) outside dataMutex
    bool ok = transmit(tx[i].buf, tx[i].n);
    LOGI("LORA", "COMMAND_TX", "socket=%s attempt=%u ok=%d", tx[i].id.c_str(), tx[i].attempt, ok);
  }
}

// Low-power (battery critical latched): after a 20 s grace for the off commands, the
// radio sleeps and wakes for a 5 s listen window every 60 s (socket poll slowed).
volatile bool g_lowPower = false;
unsigned long g_lowSince = 0;
bool g_asleep = false;
bool anyPending() {
  bool p = false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return true;
  for (auto &e : devices) if (e.second.pending) { p = true; break; }
  xSemaphoreGive(dataMutex);
  return p;
}
bool radioSleep(bool sleep) {
  if (sleep == g_asleep) return true;
  if (xSemaphoreTake(socketsMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  if (sleep) LoRa.sleep(); else LoRa.idle();  // idle: parsePacket() re-arms single RX
  xSemaphoreGive(socketsMutex);
  g_asleep = sleep;
  LOGI("LORA", sleep ? "SLEEP" : "WAKE", "low_power=%d", (int)g_lowPower);
  return true;
}

void socketsTask(void *) {
  char buf[200];
  for (;;) {
    if (g_lowPower) {
      unsigned long t = millis() - g_lowSince;
      bool listen = t < 20000 || (t % 60000) < 5000 || anyPending();
      if (!listen) { radioSleep(true); vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    }
    radioSleep(false);
    if (xSemaphoreTake(socketsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      int size = LoRa.parsePacket();
      size_t n = 0; int rssi = 0; float snr = 0;
      if (size > 0) {
        while (LoRa.available()) { int c = LoRa.read(); if (n < sizeof buf) buf[n++] = (char)c; }
        rssi = LoRa.packetRssi(); snr = LoRa.packetSnr();
      }
      xSemaphoreGive(socketsMutex);
      if (size > 0) {
        pc::lora::SocketPacket p;
        if (pc::lora::parseSocketPacket(buf, n, p)) handle(p, rssi, snr);  // non-socket frames ignored
      }
    }
    serviceCommands();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}  // namespace

void socketsInit() {
  dataMutex = xSemaphoreCreateMutex();
  socketsMutex = periph::spiMutex();  // periph::begin() runs before socketsInit()
  if (!dataMutex || !socketsMutex) ESP.restart();
  session = esp_random() & 0xFFFFFF;
}
void socketsStart() {
  ready = periph::loraPresent();  // radio brought up by periph::begin() (lora_link.cpp)
  if (!ready) { LOGE("LORA", "SOCKETS_OFF", "SX1278 not present; smart sockets unavailable"); return; }
  xSemaphoreTake(socketsMutex, portMAX_DELAY); LoRa.idle(); xSemaphoreGive(socketsMutex);
  xTaskCreatePinnedToCore(socketsTask, "socket_link", 6144, nullptr, 2, &socketsTaskHandle, 1);
  LOGI("LORA", "SOCKETS_READY", "433 MHz sync 0x%02X polled RX", LORA_SYNC_WORD);
}
bool socketsRadioReady() { return ready; }
const char *socketTransport() { return "lora"; }
void socketsDiag(const char *tag) { LOGI("LORA", "STATUS", "stage=%s ready=%d devices=%u", tag, ready, (unsigned)devices.size()); }

bool checkRegistered(String id) { return registryHas(norm(id)); }

bool registerDevice(String id) {
  id = norm(id);
  if (!validId(id) || xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto f = devices.find(id);
  // Same rule as ESP-NOW pairing: the socket must have been heard in the last 10 s.
  bool heard = f != devices.end() && f->second.seen && millis() - f->second.reading.receivedAt < 10000;
  xSemaphoreGive(dataMutex);
  if (!heard) return false;
  Preferences r;
  if (!r.begin("registry", false)) return false;
  bool ok = r.isKey(id.c_str()) || r.putBool(id.c_str(), true) == 1;
  r.end();
  if (!ok) return false;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) { devices[id].registered = true; xSemaphoreGive(dataMutex); }
  offlineq::queue("socket_paired", id.c_str());
  LOGI("LORA", "PAIRED", "socket=%s", id.c_str());
  return true;
}

void unregisterDevice(String id) {
  id = norm(id);
  if (!validId(id)) return;
  Preferences r;
  if (r.begin("registry", false)) { if (r.isKey(id.c_str())) r.remove(id.c_str()); r.end(); }
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) { devices.erase(id); xSemaphoreGive(dataMutex); }
  // LoRa sockets keep no pairing state, so there is no hardware acknowledgement to wait for.
  offlineq::queue("socket_forgotten", id.c_str());
  LOGI("LORA", "FORGOTTEN", "socket=%s", id.c_str());
}

bool populateDeviceArray(JsonArray &array) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  unsigned long now = millis();
  for (const auto &e : devices) {
    const Device &d = e.second;
    if (!d.registered) continue;
    JsonObject o = array.add<JsonObject>();
    unsigned long age = now - d.reading.receivedAt;
    bool online = ready && d.seen && age < kOnlineMs;
    o["deviceId"] = e.first;
    o["current"] = d.seen ? d.reading.current : 0;
    o["relayState"] = d.seen ? d.reading.relayState : 0;
    if (d.seen) o["rssi"] = d.reading.rssi; else o["rssi"] = nullptr;
    if (d.seen) o["snr"] = d.snr; else o["snr"] = nullptr;
    o["last_seen_age_ms"] = d.seen ? age : 0;
    o["online"] = online;
    if (d.seen) o["last_contact_age_ms"] = age; else o["last_contact_age_ms"] = nullptr;
    o["connection_reason"] = !ready ? "collector_radio_unavailable" : online ? "connected" : "connection_lost";
    o["last_restart_reason"] = nullptr;
    o["pairing_pending"] = false;
    o["transport"] = "lora";
    if (d.fw.length()) o["firmware_version"] = d.fw; else o["firmware_version"] = nullptr;
    o["command_sequence"] = d.command;
    o["ack_sequence"] = d.ack;
    o["command_status"] = d.commandStatus;
    o["lockout"] = d.lockout;
    if (d.lockout || d.lockoutA > 0) o["lockout_trip_current"] = d.lockoutA; else o["lockout_trip_current"] = nullptr;
    o["command_type"] = d.limitCmd ? "set_limit" : d.resetCmd ? "reset_lockout" : "relay";
    if (d.limitDa) o["limit_confirmed_amps"] = d.limitDa / 10.0f; else o["limit_confirmed_amps"] = nullptr;
    if (d.lockout) o["alert"] = "lockout";
    else if (d.alertOc) o["alert"] = "overcurrent";
    else if (d.alertWarn) o["alert"] = "current_high";
    else o["alert"] = nullptr;
  }
  xSemaphoreGive(dataMutex);
  return true;
}

void populateDiscoveredSockets(JsonArray &array) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  unsigned long now = millis();
  for (const auto &e : devices) {
    const Device &d = e.second;
    if (d.registered || !d.seen || now - d.reading.receivedAt >= 10000) continue;
    JsonObject o = array.add<JsonObject>();
    o["deviceId"] = e.first;
    o["relayState"] = 0;
    o["current"] = 0;
    o["online"] = false;
    o["available_to_pair"] = true;
    o["transport"] = "lora";
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

namespace {
bool queueCommand(String id, bool turnOn, bool reset, uint8_t limit = 0) {
  id = norm(id);
  if (!ready || xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  auto f = devices.find(id);
  bool accepted = false;
  if (f != devices.end()) {
    Device &d = f->second;
    if (d.registered && d.seen && millis() - d.reading.receivedAt < kOnlineMs) {
      if (!d.pending) {
        d.command = ++nextCommand; if (!d.command) d.command = ++nextCommand;
        snprintf(d.cmdId, sizeof d.cmdId, "g%06lx-%lu", (unsigned long)session, (unsigned long)d.command);
        d.desired = turnOn; d.resetCmd = reset; d.limitCmd = limit != 0; d.cmdLimit = limit; d.pending = true; d.attempts = 0; d.lastAttempt = 0; d.commandStatus = "pending";
        accepted = true;
      } else accepted = !limit && !d.limitCmd && d.resetCmd == reset && (reset || d.desired == turnOn);
    }
  }
  xSemaphoreGive(dataMutex);
  return accepted;
}
}  // namespace
bool sendRelayCommand(String id, bool turnOn) { return queueCommand(id, turnOn, false); }
// The socket keeps the relay off after a reset; the user switches it on afterwards.
bool sendResetLockout(String id) { return queueCommand(id, false, true); }
bool sendSetLimit(String id, uint8_t da) { return da && queueCommand(id, false, false, da); }
void socketsSetLowPower(bool on) {
  if (on && !g_lowPower) g_lowSince = millis();
  g_lowPower = on;
}
#endif  // FEATURE_LORA_SOCKETS
