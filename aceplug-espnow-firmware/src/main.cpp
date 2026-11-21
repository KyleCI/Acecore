// AcePlug v0.1 smart socket over ESP-NOW, firmware 1.1.0-espnow-event (Caleb 19 Nov 2026 16:19 WAT).
// *** EVENT/DEMO BUILD ONLY. NOT FOR CUSTOMER UNITS. Keys must be rotated before customer builds. ***
// Base: reference socket 1.0.1 ("Col PL.zip" smart-socket-firmware/src/main.cpp): collector scan,
// pairing, forget, encrypted unicast, OTA hold-off, watchdog. Changes:
//  - wire format from ../lib/pc_core/src/espnow_wire.h, the same header collector 2.5.40 uses
//  - 10 A trip / 8 A warn (clear 7 A) / 15 s reclose / 3 trips in 5 min lockout, from the LoRa AcePlug
//    firmware (537d7f0, 1.0.3), replacing the 50 A trip; lockout cleared by holding BOOT (IO0) 3 s
//  - 1.0.3 current noise removal (0.70 A in quadrature, 0.15 A floor) and EmonLib settle at boot
//  - LoRa module (U3) kept deselected: NSS IO5 held high; the radio is not used
// Pins: AcePlug v0.1 CAD (same as the LoRa plug firmware): relay IO15, LED IO4, ZMCT103 CT on ADC IO35.
#include <Arduino.h>
#include <EmonLib.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <PowercellOTA.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "websocket_ca.h"
#include "diagnostics.h"
#include "espnow_wire.h"
#include "plug_logic.h"
#include "trip_lockout.h"

#if !POWERCELL_ESPNOW_EVENT_KEYS
#error "AcePlug ESP-NOW 1.1.0 is an EVENT/DEMO build: customer key provisioning is not implemented. Build with POWERCELL_ESPNOW_EVENT_KEYS=1."
#endif
#if !__has_include("espnow_pairing.h")
#error "POWERCELL_ESPNOW_EVENT_KEYS=1 needs the local ../include/espnow_pairing.h (see ../include/espnow_pairing.h.example)"
#endif
#include "espnow_pairing.h"
#ifdef ESPNOW_PAIRING_PLACEHOLDER
#error "../include/espnow_pairing.h is still the placeholder; put the fleet keys in it"
#endif
static_assert(sizeof(ESPNOW_PMK) == 16 && sizeof(ESPNOW_LMK) == 16 && sizeof(ESPNOW_AUTH_KEY) == 16, "ESP-NOW keys are 16 bytes");

namespace W = pc::espnow;

namespace {
constexpr int LED_PIN = 4;
constexpr int RELAY_PIN = 15;
constexpr int CURRENT_PIN = 35;
constexpr int LORA_NSS_PIN = 5;        // U3 RFM95 NSS: held high, radio unused
constexpr int RESET_BUTTON_PIN = 0;    // module BOOT button, active low; hold 3 s clears a lockout
constexpr unsigned long RESET_HOLD_MS = 3000;
constexpr float CURRENT_CALIBRATION = 5.1F;
constexpr unsigned long TELEMETRY_INTERVAL_MS = 5000;
constexpr unsigned long COMMAND_ACK_DELAY_MS = 10;

SemaphoreHandle_t relayMutex;
EnergyMonitor currentMonitor;
String deviceId;
bool relayOn = false;
bool overcurrent = false;   // tripped, waiting for the 15 s reclose
bool currentHigh = false;   // >= 8 A warning, clears below 7 A
unsigned long ovcTripAt = 0;
TripLockout lockout;
float lastCurrent = 0.0F;
unsigned long nextTelemetryAt = 0;
bool radioReady = false;
bool watched = false;
unsigned long lastRecovery = 0;
bool initializeRadio();
QueueHandle_t receivedPackets;
struct Received { uint8_t mac[6]; uint8_t bytes[sizeof(W::Packet)]; };
uint32_t socketSession = 0, telemetrySequence = 0, collectorSession = 0, statusSequence = 0;
uint32_t lastCommand = 0, ackSession = 0, ackSequence = 0;
bool commandRejected = false;
bool paired = false;
uint32_t candidateSession = 0;
uint8_t collectorMac[6] = {}, candidateMac[6] = {}, releasedMac[6] = {};
unsigned long lastBeaconAt = 0, nextDiscoverAt = 0, nextScanAt = 0;
uint8_t scanChannel = 1;

bool addRadioPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6); memcpy(peer.lmk, ESPNOW_LMK, 16);
  peer.ifidx = WIFI_IF_STA; peer.channel = 0; peer.encrypt = true;
  return esp_now_add_peer(&peer) == ESP_OK;
}
bool saveCollector(const uint8_t *mac) {
  Preferences prefs;
  if (!prefs.begin("socket_pair", false)) return false;
  bool saved = prefs.putBytes("collector", mac, 6) == 6;
  prefs.end(); return saved;
}
void loadCollector() {
  Preferences prefs;
  if (prefs.begin("socket_pair", true)) {
    paired = prefs.getBytesLength("collector") == 6 && prefs.getBytes("collector", collectorMac, 6) == 6;
    if (prefs.getBytesLength("released") == 6) prefs.getBytes("released", releasedMac, 6);
    prefs.end();
  }
}
bool saveLockout() {
  Preferences p;
  if (!p.begin("lockout", false)) return false;
  bool ok = p.putBool("latched", lockout.latched()) == 1;
  p.putFloat("amps", lockout.lastTripAmps());
  p.end();
  return ok;
}
void receive(const uint8_t *mac, const uint8_t *bytes, int length) {
  if (!mac || !bytes || length != (int)sizeof(W::Packet)) return;
  Received e; memcpy(e.mac, mac, 6); memcpy(e.bytes, bytes, sizeof e.bytes);
  xQueueSend(receivedPackets, &e, 0);
}
bool transmit(const uint8_t *mac, W::Packet &packet) {
  W::sign(packet, ESPNOW_AUTH_KEY);
  return esp_now_send(mac, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)) == ESP_OK;
}
void discover() {
  W::Packet packet; packet.type = W::Discover; packet.session = socketSession;
  packet.flags = W::PairingSupported | (paired ? W::AlreadyPaired : 0);
  transmit(W::Broadcast, packet);
}
bool deadlineReached(unsigned long deadline) { return static_cast<long>(millis() - deadline) >= 0; }

void applyRelay(bool on) {
  if (on && (firmwareOTA.busy() || lockout.latched() || overcurrent)) return;
  relayOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}
void persistRelay(bool on) {
  Preferences safety;
  if (safety.begin("ota_safety", false)) {
    safety.putBool("relay_on", on);
    safety.putBool("hold_off", !on);
    safety.end();
  }
}

void sendTelemetry(const char *reason = nullptr) {
  if (!radioReady || !paired || !collectorSession) return;
  W::Packet packet;
  packet.type = W::Telemetry; packet.session = socketSession;
  packet.sequence = ++telemetrySequence; packet.current = lastCurrent; packet.relay = relayOn;
  packet.flags = aceplug::telemetryFlags(commandRejected, overcurrent, lockout.latched(), static_cast<uint8_t>(esp_reset_reason()));
  packet.ackSession = ackSession; packet.ackSequence = ackSequence;
  bool sent = transmit(collectorMac, packet);
  Serial.printf("[ESPNOW] Telemetry queued=%d id=%s current=%.3fA relay=%s ack=%lu warn=%d lockout=%d%s%s\n",
                sent, deviceId.c_str(), lastCurrent, relayOn ? "ON" : "OFF", (unsigned long)ackSequence,
                currentHigh, lockout.latched(), reason ? " reason=" : "", reason ? reason : "");
}
// Optional Status packet on alert changes (collector 2.5.40 ignores it; telemetry carries overcurrent).
void sendStatus() {
  if (!radioReady || !paired || !collectorSession) return;
  W::Packet p;
  p.type = W::Status; p.session = socketSession; p.sequence = ++statusSequence;
  p.flags = aceplug::statusFlags(currentHigh, lockout.latched(), overcurrent);
  p.relay = relayOn; p.current = lockout.latched() ? lockout.lastTripAmps() : lastCurrent;
  p.ackSession = W::packInfo(100, ACEPLUG_FW_MAJOR, ACEPLUG_FW_MINOR, ACEPLUG_FW_PATCH);
  p.ackSequence = 0;
  transmit(collectorMac, p);
}

void handleRelayCommand(bool requestedOn) {
  if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(200)) != pdTRUE) { diagnostic("ERROR", "COMMAND", "RELAY_MUTEX_TIMEOUT", "device=%s wait_ms=200", deviceId.c_str()); commandRejected = true; return; }
  if (requestedOn && (overcurrent || lockout.latched() || firmwareOTA.busy())) {
    xSemaphoreGive(relayMutex);
    diagnostic("WARN", "COMMAND", "ON_REJECTED", "device=%s overcurrent=%d lockout=%d ota_busy=%d", deviceId.c_str(), overcurrent, lockout.latched(), firmwareOTA.busy());
    commandRejected = true;
    return;
  }
  persistRelay(requestedOn);
  applyRelay(requestedOn);
  xSemaphoreGive(relayMutex);
  Serial.printf("[ESPNOW] Command applied: %s\n", requestedOn ? "ON" : "OFF");
}

void pollCommands() {
  Received e;
  while (xQueueReceive(receivedPackets, &e, 0) == pdTRUE) {
    W::Packet packet;
    if (!W::decode(e.bytes, sizeof e.bytes, packet, ESPNOW_AUTH_KEY)) continue;
    if (packet.type == W::Forget &&
        W::acceptsForget(paired, !memcmp(e.mac, collectorMac, 6), !memcmp(e.mac, releasedMac, 6),
                         packet.session, collectorSession, packet.sequence, lastCommand)) {
      if (firmwareOTA.networkBusy()) continue;
      if (paired) {
        persistRelay(false); applyRelay(false);
        if (!firmwareOTA.clearConfiguration()) continue;
        Preferences prefs;
        if (!prefs.begin("socket_pair", false)) continue;
        bool saved = prefs.putBytes("released", e.mac, 6) == 6;
        if (saved && prefs.getBytesLength("collector") == 6) saved = prefs.remove("collector");
        prefs.end();
        if (!saved) continue;
        memcpy(releasedMac, e.mac, 6);
        paired = false; collectorSession = 0; ackSession = ackSequence = lastCommand = 0;
        socketSession = esp_random(); if (!socketSession) socketSession = 1;
        Serial.println("[ESPNOW] Forgotten; ready to add as a new socket");
      }
      if (addRadioPeer(e.mac)) {
        W::Packet ack; ack.type = W::ForgetAck; ack.session = socketSession;
        ack.ackSession = packet.session; ack.ackSequence = packet.sequence;
        transmit(e.mac, ack);
      }
      continue;
    }
    if (paired && memcmp(e.mac, collectorMac, 6)) continue;
    if (packet.type == W::Pair) {
      if (!W::acceptsPair(paired, !memcmp(e.mac, collectorMac, 6), !memcmp(e.mac, candidateMac, 6),
                          packet.session, collectorSession, candidateSession, packet.ackSession, socketSession, millis() - lastBeaconAt)) continue;
      if (!addRadioPeer(e.mac) || (!paired && !saveCollector(e.mac))) continue;
      memcpy(collectorMac, e.mac, 6); paired = true; collectorSession = packet.session;
      if (W::newer(packet.sequence, lastCommand)) lastCommand = packet.sequence;
      W::Packet ack; ack.type = W::PairAck; ack.session = socketSession;
      ack.ackSession = packet.session; ack.ackSequence = packet.sequence;
      transmit(collectorMac, ack); sendTelemetry("paired");
      diagnostic("INFO", "ESPNOW", "PAIRED", "collector=%02X%02X%02X%02X%02X%02X channel=%d", e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5], WiFi.channel());
      continue;
    }
    if (packet.type == W::Beacon) {
      if (!paired) {
        if (memcmp(candidateMac, e.mac, 6)) {
          if (memcmp(candidateMac, releasedMac, 6) && memcmp(candidateMac, collectorMac, 6)) esp_now_del_peer(candidateMac);
          memcpy(candidateMac, e.mac, 6);
        }
        if (addRadioPeer(e.mac)) { candidateSession = packet.session; lastBeaconAt = millis(); discover(); }
        continue;
      }
      bool changed = collectorSession != packet.session;
      if (changed) { collectorSession = packet.session; lastCommand = 0; ackSession = 0; ackSequence = 0; }
      lastBeaconAt = millis();
      if (changed) {
        diagnostic("INFO", "ESPNOW", "COLLECTOR_FOUND", "channel=%d session=%lu", WiFi.channel(), (unsigned long)collectorSession);
        discover(); sendTelemetry("paired");
      }
    } else if (packet.type == W::Command && packet.session == collectorSession && collectorSession) {
      if (packet.sequence == lastCommand && ackSession == collectorSession && ackSequence == lastCommand) {
        sendTelemetry("duplicate_ack"); continue;
      }
      if (!W::newer(packet.sequence, lastCommand)) continue;
      lastCommand = packet.sequence; ackSession = collectorSession; ackSequence = packet.sequence;
      commandRejected = false;
      handleRelayCommand(packet.relay);
      if (relayOn != bool(packet.relay)) commandRejected = true;
      delay(COMMAND_ACK_DELAY_MS);
      sendTelemetry(commandRejected ? "command_rejected" : "command_ack");
      nextTelemetryAt = millis() + TELEMETRY_INTERVAL_MS + random(0, 500);
    }
  }
  if (firmwareOTA.networkBusy()) return;  // OTA temporarily owns the Wi-Fi channel.
  const bool observedCollector = paired ? collectorSession != 0 : candidateSession != 0;
  if ((!observedCollector || millis() - lastBeaconAt > 3500) && millis() >= nextScanAt) {
    esp_wifi_set_channel(scanChannel, WIFI_SECOND_CHAN_NONE);
    if (++scanChannel > 13) scanChannel = 1;
    nextScanAt = millis() + 300;
  }
  if (millis() >= nextDiscoverAt) { discover(); nextDiscoverAt = millis() + 1000; }
}

bool initializeRadio() {
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t collector = {};
  memcpy(collector.peer_addr, collectorMac, 6);
  memcpy(collector.lmk, ESPNOW_LMK, 16);
  collector.ifidx = WIFI_IF_STA; collector.channel = 0; collector.encrypt = true;
  esp_now_peer_info_t broadcast = {};
  memcpy(broadcast.peer_addr, W::Broadcast, 6);
  broadcast.ifidx = WIFI_IF_STA; broadcast.channel = 0; broadcast.encrypt = false;
  radioReady = esp_now_set_pmk(ESPNOW_PMK) == ESP_OK
      && (!paired || esp_now_add_peer(&collector) == ESP_OK) && esp_now_add_peer(&broadcast) == ESP_OK
      && esp_now_register_recv_cb(receive) == ESP_OK;
  if (!radioReady) esp_now_deinit();
  diagnostic(radioReady ? "INFO" : "ERROR", "ESPNOW", "READY", "socket=%s channel=%d encrypted_unicast=1 event_keys=1", deviceId.c_str(), WiFi.channel());
  return radioReady;
}

void sampleCurrent() {
  const unsigned long now = millis();
  lastCurrent = aceplug::denoise(static_cast<float>(currentMonitor.calcIrms(1480)));
  const char *reason = nullptr;
  bool statusEdge = false;
  if (aceplug::shouldTrip(lastCurrent) && !overcurrent && !lockout.latched()) {
    overcurrent = true; ovcTripAt = now;
    digitalWrite(RELAY_PIN, LOW); digitalWrite(LED_PIN, LOW); relayOn = false;
    reason = "overcurrent_trip"; statusEdge = true;
    diagnostic("ERROR", "SAFETY", "OVERCURRENT_TRIP", "current_a=%.3f limit_a=%.1f; relay OFF, reclose in 15 s", lastCurrent, aceplug::kTripAmps);
    if (lockout.onTrip(now, lastCurrent)) {
      overcurrent = false; ovcTripAt = 0;  // no auto reclose
      const bool saved = saveLockout();
      persistRelay(false);
      reason = "lockout";
      diagnostic("ERROR", "SAFETY", "LOCKOUT", "3 trips in 5 min, last %.2f A; relay latched off (saved=%d). Hold BOOT 3 s to clear", lastCurrent, saved);
    }
  }
  const bool high = aceplug::warnState(currentHigh, lastCurrent);
  if (high != currentHigh) {
    currentHigh = high; statusEdge = true;
    if (!reason) reason = high ? "current_high" : "current_normal";
    diagnostic(high ? "WARN" : "INFO", "SAFETY", high ? "CURRENT_HIGH" : "CURRENT_NORMAL", "current_a=%.3f warn_a=%.1f clear_a=%.1f", lastCurrent, aceplug::kWarnAmps, aceplug::kWarnClearAmps);
  }
  sendTelemetry(reason);
  if (statusEdge) sendStatus();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  logBoot("socket", PowercellOTA::version());
  Serial.println("[ACEPLUG] EVENT/DEMO BUILD ONLY - not for customer units (built-in event keys)");
  digitalWrite(LORA_NSS_PIN, HIGH); pinMode(LORA_NSS_PIN, OUTPUT); digitalWrite(LORA_NSS_PIN, HIGH);
  loadCollector();
  relayMutex = xSemaphoreCreateMutex();
  if (!relayMutex) { pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW); ESP.restart(); }
  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  Preferences lk;
  if (lk.begin("lockout", true)) { lockout.restore(lk.getBool("latched", false), lk.getFloat("amps", 0.0F)); lk.end(); }
  if (lockout.latched()) diagnostic("WARN", "SAFETY", "LOCKOUT", "latched from NVS (last trip %.2f A); relay stays off", lockout.lastTripAmps());
  Preferences safety;
  safety.begin("ota_safety", true);
  bool holdOff = safety.getBool("hold_off", false);
  bool savedRelay = safety.getBool("relay_on", false);
  safety.end();
  applyRelay(paired && savedRelay && !holdOff && !lockout.latched());
  firmwareOTA.begin(true, WEBSOCKET_CA_CERT, []() {
    if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    Preferences s;
    bool stored = false;
    if (s.begin("ota_safety", false)) { stored = s.putBool("hold_off", true) == 1; s.end(); }
    applyRelay(false);
    xSemaphoreGive(relayMutex);
    return stored;
  });

  WiFi.mode(WIFI_STA);
  deviceId = WiFi.macAddress();
  deviceId.replace(":", "");
  receivedPackets = xQueueCreate(16, sizeof(Received));
  if (!receivedPackets) ESP.restart();
  socketSession = esp_random(); if (!socketSession) socketSession = 1;

  randomSeed(esp_random());
  currentMonitor.current(CURRENT_PIN, CURRENT_CALIBRATION);
  for (int i = 0; i < 10; ++i) currentMonitor.calcIrms(1480);  // settle EmonLib's offset filter on the ZMCT103 bias
  initializeRadio();
  watched = esp_task_wdt_init(15, true) == ESP_OK && esp_task_wdt_add(nullptr) == ESP_OK;
  if (!watched) Serial.println("[ESPNOW] Watchdog registration failed");
  nextTelemetryAt = millis() + 1000 + random(0, 500);
  Serial.printf("[ESPNOW] AcePlug %s ready id=%s; searching for collector (trip %.0f A, warn %.0f A)\n",
                PowercellOTA::version(), deviceId.c_str(), aceplug::kTripAmps, aceplug::kWarnAmps);
}

void loop() {
  if (!firmwareOTA.networkBusy() && WiFi.getMode() == WIFI_OFF) {
    radioReady = false; esp_now_deinit(); collectorSession = 0;
  }
  if (!firmwareOTA.networkBusy() && !radioReady && millis() - lastRecovery >= 5000) {
    lastRecovery = millis();
    initializeRadio();
  }
  pollCommands();
  firmwareOTA.service();
  static String serialLine;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n') { firmwareOTA.configure(serialLine); serialLine = ""; }
    else if (c != '\r') {
      if (serialLine.length() < 512) serialLine += c;
      else serialLine = "";
    }
  }
  const unsigned long now = millis();
  // BOOT button held 3 s: clear a lockout (relay stays off; the user switches it on).
  static unsigned long pressedAt = 0;
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!pressedAt) pressedAt = now ? now : 1;
    else if (pressedAt != ULONG_MAX && now - pressedAt >= RESET_HOLD_MS) {
      pressedAt = ULONG_MAX;
      if (lockout.latched() && xSemaphoreTake(relayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lockout.reset(); overcurrent = false; ovcTripAt = 0;
        const bool saved = saveLockout();
        xSemaphoreGive(relayMutex);
        diagnostic("INFO", "SAFETY", "LOCKOUT_RESET", "by button (saved=%d)", saved);
        sendTelemetry("lockout_reset"); sendStatus();
      }
    }
  } else pressedAt = 0;
  // 15 s after a trip (not locked out): reclose and tell the collector.
  if (overcurrent && !lockout.latched() && now - ovcTripAt >= aceplug::kRecloseMs) {
    overcurrent = false; ovcTripAt = 0;
    applyRelay(true);
    diagnostic("INFO", "SAFETY", "RECLOSED", "relay restored 15 s after the trip");
    sendTelemetry("power_restored"); sendStatus();
  }
  if (deadlineReached(nextTelemetryAt)) {
    sampleCurrent();
    nextTelemetryAt = millis() + TELEMETRY_INTERVAL_MS + random(0, 500);
  }
  if (watched) esp_task_wdt_reset();
  delay(1);
}
