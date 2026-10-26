#include <mbedtls/error.h>
#include "json_scratch.h"
#include "net.h"
#include "socket_cfg.h"
#include "power_guard.h"
#include "backend_ws.h"
#include "events.h"
#include <WebSocketsClient.h>
#include <errno.h>
#include <WiFi.h>
#include <time.h>
#include "ca_cert.h"
#include "config.h"
#include "gateway.h"
#include "identity.h"
#include "local_auth.h"
#include "log.h"
#include "offline_queue.h"
#include "onboarding.h"
#include "ota.h"
#include "heapguard.h"
#include "settings.h"
#include "sockets/sockets.h"
#include "telemetry.h"
#include "web_local.h"
#include "devauth.h"
#include "factory_id.h"
#include "payment_lock.h"
#include "clock.h"

namespace backend {
namespace {
// v11: disconnect diagnostics. The WebSockets library hides the close code and drops
// the TLS client before WStype_DISCONNECTED, so the two virtual hooks record why first:
//  - clientIsConnected(): transport lost (TCP/TLS read failed, e.g. mbedTLS -76
//    NET_RECV_FAILED): errno and WiFiClientSecure::lastError() are captured here.
//  - clientDisconnect(client): close frame from the server, protocol error, or our
//    heartbeat timeout (pongTimeoutCount >= limit).
struct DropInfo { const char *path = "unknown"; int err = 0; int tls = 0; char tlsMsg[64] = {0}; uint8_t pongMisses = 0; };
DropInfo g_drop;
bool g_localClose = false;  // pauseForOta / wifi-test hold: our own disconnect
uint32_t g_lastRx = 0;
class DiagWsClient : public WebSocketsClient {
 protected:
  // v12: WiFiClientSecure::lastError() only covers the handshake, and a receive failure
  // (ssl_client data_to_read(): -76 NET_RECV_FAILED) is returned once by available()
  // and then the socket is stopped. Probe available() here, before the library does, so
  // the mbedTLS code and errno are kept before the client is torn down.
  int probeErr_ = 0, probeErrno_ = 0;
  void capture(WSclient_t *c, const char *path) {
    if (g_drop.path && strcmp(g_drop.path, "unknown")) return;  // first cause wins
    g_drop = DropInfo();
    g_drop.path = path; g_drop.err = probeErrno_ ? probeErrno_ : errno; g_drop.pongMisses = c->pongTimeoutCount;
    g_drop.tls = probeErr_;
#if defined(HAS_SSL)
    if (!g_drop.tls && c->isSSL && c->ssl) g_drop.tls = c->ssl->lastError(g_drop.tlsMsg, sizeof g_drop.tlsMsg);
#endif
    if (g_drop.tls && !g_drop.tlsMsg[0]) mbedtls_strerror(g_drop.tls, g_drop.tlsMsg, sizeof g_drop.tlsMsg);
  }
  bool clientIsConnected(WSclient_t *c) override {
    if (c->tcp && c->status != WSC_NOT_CONNECTED) {
      errno = 0;
      int a = c->tcp->available();
      if (a < 0 && !probeErr_) { probeErr_ = a; probeErrno_ = errno; }
      if (!c->tcp->connected()) capture(c, "transport_lost");
    }
    return WebSocketsClient::clientIsConnected(c);
  }
 public:
  void resetProbe() { probeErr_ = probeErrno_ = 0; }
 protected:
  void clientDisconnect(WSclient_t *c) override {
    if (c->status != WSC_NOT_CONNECTED)
      capture(c, c->disconnectTimeoutCount && c->pongTimeoutCount >= c->disconnectTimeoutCount ? "heartbeat_timeout" : "close_frame_or_protocol");
    WebSocketsClient::clientDisconnect(c, static_cast<const char *>(nullptr));
  }
};
DiagWsClient ws;
SemaphoreHandle_t g_app = nullptr, g_wsMx = nullptr;  // both recursive; order: app before ws
struct AppHold { AppHold() { appLock(); } ~AppHold() { appUnlock(); } };
bool wsTake(uint32_t ms) { return xSemaphoreTakeRecursive(g_wsMx, pdMS_TO_TICKS(ms)) == pdTRUE; }
void wsGive() { xSemaphoreGiveRecursive(g_wsMx); }
String g_headers;  // WebSocketsClient keeps the pointer: must outlive the connection
String g_path;
bool g_inited = false, g_started = false;
bool g_hdrDirty = true; uint32_t g_hdrAt = 0, g_wifiUpAt = 0;
String g_apiKey;
volatile bool g_connected = false;
bool g_telemetryNow = false;
uint32_t g_lastTelemetry = 0, g_lastSnapshot = 0, g_connectedAt = 0, g_reconnects = 0, g_lastNtpLog = 0;
bool g_timeLogged = false;
// 4401 handling: the WebSockets library hides the close code, so a server close
// within kQuickCloseMs of the upgrade counts as an auth rejection (the dev server
// closes unauthenticated sockets with 4401 right away). Reconnects then back off
// 5 s, 10 s, 20 s ... up to 5 min instead of hammering the server every 5 s.
constexpr uint32_t kQuickCloseMs = 8000, kRetryMs = 5000, kRetryMaxMs = 300000;
uint8_t g_quickCloses = 0;
pc::TlsBackoff g_tlsBackoff;  // memory guard for the WS TLS (re)connect

struct Pending {
  bool active = false;
  String id, socketId;
  bool desired = false;
  bool reset = false;  // reset_lockout command
  uint32_t seq = 0;  // socket command sequence assigned by the radio layer
  uint32_t issuedAt = 0, deadline = 0, lastAttempt = 0;
};
constexpr int kMaxPending = 4;
Pending pending[kMaxPending];

constexpr int kRecent = 8;  // recently completed command ids, for idempotent retries
String recentIds[kRecent];
String recentResults[kRecent];
int recentNext = 0;

uint64_t unixNow() {
  time_t now = time(nullptr);
  return now >= 1700000000 ? static_cast<uint64_t>(now) : 0;
}

void remember(const String &id, const char *result) {
  recentIds[recentNext] = id;
  recentResults[recentNext] = result;
  recentNext = (recentNext + 1) % kRecent;
}
const String *recentResult(const String &id) {
  for (int i = 0; i < kRecent; ++i) if (recentIds[i] == id) return &recentResults[i];
  return nullptr;
}

const char *g_action = nullptr;  // set around reset_lockout results so they carry "action"
void result(const String &id, const char *targetType, const String &target, bool desired, const char *status,
            const char *error = nullptr, bool record = true) {
  JsonDocument doc;
  doc["type"] = "command_result";
  addIdentity(doc);
  doc["command_id"] = id;
  doc["target_type"] = targetType;
  doc["target_id"] = target;
  doc["desired_state"] = desired ? "on" : "off";
  doc["reported_state"] = desired ? "on" : "off";
  doc["result"] = status;
  if (g_action) doc["action"] = g_action;
  // Backend PR #3 reads command_status ?? status and accepts only confirmed|rejected.
  const char *wire = !strcmp(status, "duplicate") ? (error && !strcmp(error, "confirmed") ? "confirmed" : "rejected") : status;
  doc["status"] = wire;
  doc["command_status"] = wire;
  if (error) { doc["error"] = error; doc["reason"] = error; doc["error_message"] = error; }
  send(doc);
  if (record) remember(id, status);
}

void reject(const String &id, const String &kind, const String &target, bool desired, const char *reason) {
  LOGW("WS", "REJECT", "id=%s target=%s reason=%s", id.c_str(), target.c_str(), reason);
  result(id, kind.c_str(), target, desired, "rejected", reason);
}

void snapshot() {
  JsonDocument doc;
  doc["type"] = "state_snapshot";
  addIdentity(doc);
  telemetry::buildSnapshot(doc);
  send(doc);
  g_lastSnapshot = millis();
}

void sendTelemetry() {
  JsonDocument doc;
  doc["type"] = "telemetry";
  addIdentity(doc);
  telemetry::buildTelemetry(doc);
  size_t nev = events::appendPending(doc["events"].to<JsonArray>());
  if (send(doc)) events::commitSent(nev);
  g_lastTelemetry = millis();
}

bool isGatewayTarget(const String &t) {
  const auto &id = settings::id();
  // Backend pushes target_id null for gateway commands: that means this unit.
  return t.isEmpty() || t.equalsIgnoreCase(id.gatewayName) || pc::sameDeviceId(t.c_str(), id.deviceId);
}

// {"type":"command","command_id","action":"reset_lockout","target_type":"socket","target_id":"<socket>"}
// Clears the socket's 3-trips lockout; the relay stays off (desired/reported_state "off").
void handleResetLockout(const String &id, const String &kind, const String &target, JsonDocument &doc) {
  if (const String *prev = recentResult(id)) { result(id, kind.c_str(), target, false, "duplicate", prev->c_str(), false); return; }
  for (auto &p : pending) if (p.active && p.id == id) return;
  if (kind != "socket") { reject(id, kind, target, false, "invalid_target_type"); return; }
  uint64_t expires = doc["expires_at"] | uint64_t(0);
  if (expires && unixNow() && expires <= unixNow()) { reject(id, kind, target, false, "command_expired"); return; }
  if (!target.length() || !checkRegistered(target)) { reject(id, kind, target, false, "unknown_target"); return; }
  Pending *slot = nullptr;
  for (auto &p : pending) {
    if (p.active && p.socketId.equalsIgnoreCase(target)) { reject(id, kind, target, false, "socket_busy"); return; }
    if (!p.active && !slot) slot = &p;
  }
  if (!slot) { reject(id, kind, target, false, "command_queue_full"); return; }
  if (!sendResetLockout(target)) { reject(id, kind, target, false, "socket_offline"); return; }
  slot->seq = socketCommandSequence(target);
  slot->active = true; slot->id = id; slot->socketId = target; slot->desired = false; slot->reset = true;
  slot->issuedAt = slot->lastAttempt = millis(); slot->deadline = slot->issuedAt + 7000;
  LOGI("WS", "ACCEPTED", "id=%s socket=%s action=reset_lockout", id.c_str(), target.c_str());
}

void handleSetState(JsonDocument &doc) {
  String id = doc["command_id"] | "";
  String kind = doc["target_type"] | "";
  String target;
  if (doc["target_id"].is<const char *>()) target = doc["target_id"].as<String>();
  else if (doc["target_id"].is<long>()) target = String(doc["target_id"].as<long>());
  String desiredText = doc["desired_state"] | "";
  bool desired = desiredText == "on";
  if (!id.length() || id.length() > 47) { reject(id, kind, target, desired, "invalid_command_id"); return; }
  String action = doc["action"] | "";
  if (action == "reset_lockout") {
    g_action = "reset_lockout";
    handleResetLockout(id, kind, target, doc);
    g_action = nullptr;
    return;
  }
  if (action == "set_limit" || action == "set_config" || action == "set_override") {
    // Socket config: stored (and applied by the local engine) synchronously, so the result is final.
    // The LoRa SET_LIMIT ack shows later in telemetry (limit_confirmed_amps, limit_pending).
    g_action = action == "set_limit" ? "set_limit" : action == "set_config" ? "set_config" : "set_override";
    if (const String *prev = recentResult(id)) result(id, "socket", target, false, "duplicate", prev->c_str(), false);
    else {
      String type = action; type.toUpperCase();
      JsonObjectConst body = doc["body"].is<JsonObjectConst>() ? doc["body"].as<JsonObjectConst>() : doc.as<JsonObjectConst>();
      String sock;
      const char *err = socketcfg::apply(body, type.c_str(), target, sock);
      if (err) reject(id, "socket", sock, false, err);
      else { LOGI("WS", "ACCEPTED", "id=%s socket=%s action=%s", id.c_str(), sock.c_str(), g_action); result(id, "socket", sock, false, "confirmed"); requestTelemetry(); }
    }
    g_action = nullptr;
    return;
  }
  if (action.length() && action != "set_state") { reject(id, kind, target, desired, "invalid_action"); return; }
  if (const String *prev = recentResult(id)) {  // retry of a command we already handled
    result(id, kind.c_str(), target, desired, "duplicate", prev->c_str(), false);
    return;
  }
  for (auto &p : pending) if (p.active && p.id == id) return;  // still in progress
  if (desiredText != "on" && desiredText != "off") { reject(id, kind, target, false, "invalid_state"); return; }
  if (kind != "gateway" && kind != "socket") { reject(id, kind, target, desired, "invalid_target_type"); return; }
  uint64_t expires = doc["expires_at"] | uint64_t(0);
  if (expires && unixNow() && expires <= unixNow()) { reject(id, kind, target, desired, "command_expired"); return; }
  if (ota::busy()) { reject(id, kind, target, desired, "firmware_update_in_progress"); return; }
  if (desired && powerguard::latched()) { reject(id, kind, target, desired, "battery_critical_latched"); return; }
  if (fid::paymentLocked()) { reject(id, kind, target, desired, "payment_locked"); return; }

  if (kind == "gateway") {
    if (!isGatewayTarget(target)) { reject(id, kind, target, desired, "unknown_target"); return; }
    gateway::Result gr = gateway::set(desired, "cloud");  // framed: waits for the STM32 set_mode ACK
    if (gr != gateway::Result::Ok) { reject(id, kind, target, desired, gateway::resultName(gr)); return; }
    result(id, "gateway", target, desired, "confirmed");
    snapshot();
    return;
  }
  if (!target.length() || !checkRegistered(target)) { reject(id, kind, target, desired, "unknown_target"); return; }
  Pending *slot = nullptr;
  for (auto &p : pending) {
    if (p.active && p.socketId.equalsIgnoreCase(target)) { reject(id, kind, target, desired, "socket_busy"); return; }
    if (!p.active && !slot) slot = &p;
  }
  if (!slot) { reject(id, kind, target, desired, "command_queue_full"); return; }
  if (!sendRelayCommand(target, desired)) { reject(id, kind, target, desired, "socket_offline"); return; }
  slot->seq = socketCommandSequence(target);
  slot->active = true; slot->id = id; slot->socketId = target; slot->desired = desired; slot->reset = false;
  slot->issuedAt = slot->lastAttempt = millis(); slot->deadline = slot->issuedAt + 7000;
  LOGI("WS", "ACCEPTED", "id=%s socket=%s desired=%s", id.c_str(), target.c_str(), desired ? "on" : "off");
  snapshot();
}

void servicePending() {
  bool any = false;
  for (auto &p : pending) any |= p.active;
  if (!any) return;
  JsonDocument tmp;
  JsonArray sockets = tmp.to<JsonArray>();
  populateDeviceArray(sockets);
  uint32_t now = millis();
  for (auto &p : pending) {
    if (!p.active) continue;
    g_action = p.reset ? "reset_lockout" : nullptr;
    for (JsonObject s : sockets) {
      if (!p.socketId.equalsIgnoreCase(s["deviceId"] | "")) continue;
      if ((s["command_sequence"] | 0u) != p.seq) break;  // status belongs to an older command
      String st = s["command_status"] | "";
      if (st == "confirmed" && (p.reset ? !(s["lockout"] | false) : (s["relayState"].as<int>() == 1) == p.desired)) {
        result(p.id, "socket", p.socketId, p.desired, "confirmed"); p.active = false;
      } else if (st == "rejected" || st == "timeout") {
        result(p.id, "socket", p.socketId, p.desired, "rejected", st == "timeout" ? "hardware_timeout" : "hardware_rejected"); p.active = false;
      }
      break;
    }
    if (p.active && (int32_t)(now - p.deadline) >= 0) {
      result(p.id, "socket", p.socketId, p.desired, "rejected", "hardware_timeout");
      p.active = false;
    }
    g_action = nullptr;
    if (!p.active) snapshot();
  }
}

// Local LAN token handoff (proposed to the backend; see README "Local token handoff").
// In : {"type":"set_local_token","request_id":"<<=47>","username":"powercell","token":"<32..128 [0-9A-Za-z_-]>"}
// Out: {"type":"local_token_ack","request_id":..,"status":"confirmed"|"rejected","error":..,
//       "token_fingerprint":"<16 hex of sha256(token)>", + identity}
// Only over TLS (the token is a credential); idempotent for the same token.
void handleSetLocalToken(JsonDocument &doc) {
  String rid = doc["request_id"] | "";
  String user = doc["username"] | pc::kLocalUser;
  String token = doc["token"] | "";
  const char *err = nullptr;
  if (!rid.length() || rid.length() > 47) err = "invalid_request_id";
  else if (!POWERCELL_WS_TLS) err = "insecure_transport";
  else if (user != pc::kLocalUser) err = "invalid_username";
  else if (!pc::validLocalToken(token.c_str())) err = "invalid_token";
  else if (token != settings::localToken() && !settings::setLocalToken(token)) err = "storage_failed";
  if (!err) web::tokenChanged();
  JsonDocument ack;
  ack["type"] = "local_token_ack";
  addIdentity(ack);
  ack["request_id"] = rid;
  ack["status"] = err ? "rejected" : "confirmed";
  if (err) ack["error"] = err;
  ack["token_fingerprint"] = settings::localTokenFingerprint();
  send(ack);
  LOGI("WS", "LOCAL_TOKEN", "request=%s result=%s fp=%s", rid.c_str(), err ? err : "confirmed", settings::localTokenFingerprint().c_str());
}

void sendHello() {
  if (!g_connected) return;
  JsonDocument doc;
  onboarding::buildHello(doc);
  addIdentity(doc);
  if (send(doc)) onboarding::helloSent();
  JsonDocument pc_;
  if (onboarding::takePasscodeChanged(pc_)) { addIdentity(pc_); send(pc_); }
}

// v15 remote factory reset / v16 HQ payment_lock (signed cmds).
// factory_reset: sig = HMAC(secret, cmd_id\nreason\nissued_at)
// payment_lock:  sig = HMAC(secret, cmd_id\n(locked?1:0)\nreason\nissued_at)
// Ack: cmd_ack with accepted bool (payment_lock.md) plus status/error (v15 factory_reset style)
// so the server resend pump (FactoryService::acknowledge) stays happy.
void handleCmd(JsonDocument &doc) {
  String cmd = doc["cmd"] | "", id = doc["cmd_id"] | "", reason = doc["reason"] | "", sig = doc["sig"] | "";
  uint32_t issued = doc["issued_at"] | 0u;
  bool locked = doc["locked"] | false;
  const char *err = nullptr;
  bool apply = false;
  if (cmd != "factory_reset" && cmd != "payment_lock") err = "unknown_cmd";
  else if (!id.length() || id.length() > 64 || reason.length() > 64 || sig.length() != 64 || !issued) err = "invalid_request";
  else if (!fid::hasSecret()) err = "no_secret";
  else if (cmd == "factory_reset") {
    String msg = id + "\n" + reason + "\n" + String(issued);
    uint8_t mac[32]; char hex[65];
    fid::hmac(reinterpret_cast<const uint8_t *>(msg.c_str()), msg.length(), mac);
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    String s = sig; s.toLowerCase();
    if (!pc::ctEquals(hex, 64, s.c_str(), 64)) err = "bad_signature";
    else if (fid::cmdSeen(id)) err = "duplicate";
    else if (issued <= fid::lastResetAt()) err = "stale";
    else apply = true;
  } else {
    char msg[160];
    size_t n = pc::paylock::message(id.c_str(), locked, reason.c_str(), issued, msg, sizeof msg);
    uint8_t mac[32]; char hex[65];
    if (!n || !fid::hmac(reinterpret_cast<const uint8_t *>(msg), n, mac)) err = "invalid_request";
    else {
      for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
      String s = sig; s.toLowerCase();
      if (!pc::ctEquals(hex, 64, s.c_str(), 64)) err = "bad_signature";
      else {
        auto v = pc::paylock::judge(issued, fid::lastPaymentLockAt(), fid::paymentCmdSeen(id));
        if (v == pc::paylock::Verdict::Duplicate) err = "duplicate";
        else if (v == pc::paylock::Verdict::Stale) err = "stale";
        else if (v == pc::paylock::Verdict::Invalid) err = "invalid_request";
        else apply = true;
      }
    }
  }
  bool done = apply || (err && !strcmp(err, "duplicate"));
  JsonDocument ack;
  ack["type"] = "cmd_ack";
  addIdentity(ack);
  ack["cmd_id"] = id;
  ack["accepted"] = done;
  ack["status"] = done ? "accepted" : "rejected";
  if (err) ack["error"] = err; else ack["error"] = nullptr;
  send(ack);
  LOGW("WS", "CMD", "cmd=%s id=%s result=%s", cmd.c_str(), id.c_str(), err ? err : "accepted");
  if (!apply) return;
  if (cmd == "factory_reset") onboarding::factoryReset("remote", id, issued);
  else {
    bool was = fid::paymentLocked();
    if (!fid::applyPaymentLock(locked, reason, id, issued)) {
      LOGE("WS", "PAYMENT_LOCK", "NVS apply failed id=%s", id.c_str());
      return;
    }
    LOGW("WS", "PAYMENT_LOCK", "locked=%d reason_len=%u was=%d", (int)locked, (unsigned)reason.length(), (int)was);
    // Financing lock must not leave the inverter running; unlock does not auto-ON.
    if (locked && !was) gateway::set(false, "payment_lock");
    requestTelemetry();
    snapshot();
  }
}

void sendResetEvent() {
  fid::ResetInfo ri = fid::resetInfo();
  if (!ri.pending) return;
  JsonDocument ev;
  ev["type"] = "event";
  addIdentity(ev);
  ev["event"] = "factory_reset";
  ev["serial"] = fid::serial();
  ev["source"] = ri.source;
  if (ri.cmdId.length()) ev["cmd_id"] = ri.cmdId; else ev["cmd_id"] = nullptr;
  ev["fw_version"] = ota::version();
  ev["reset_at"] = ri.at;
  send(ev);
}

void incoming(uint8_t *payload, size_t length) {
  if (!payload || !length || length > WS_MAX_MESSAGE) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) || !doc.is<JsonObject>()) return;
  String type = doc["type"] | "";
  if (type == "ping") { JsonDocument pong; pong["type"] = "pong"; addIdentity(pong); send(pong); }
  else if (type == "offline_sync_ack") offlineq::acknowledge(doc["event_ids"].as<JsonArrayConst>());
  else if (type == "socket_lifecycle") {
    for (JsonVariantConst v : doc["forget_ids"].as<JsonArrayConst>()) unregisterDevice(v.as<String>());
    for (JsonVariantConst v : doc["pair_ids"].as<JsonArrayConst>()) { String sid = v.as<String>(); if (!checkRegistered(sid)) registerDevice(sid); }
  } else if (type == "set_state" || type == "command") handleSetState(doc);  // "command" = PR #3 wire name
  else if (type == "set_local_token") handleSetLocalToken(doc);
  else if (type == "set_passcode") {  // wss only (POWERCELL_WS_TLS), idempotent on request_id
    JsonDocument ack;
    onboarding::handleSetPasscode(doc, ack, POWERCELL_WS_TLS);
    addIdentity(ack);
    send(ack);
    sendHello();
  } else if (type == "claim_result") { onboarding::handleClaimResult(doc); sendHello(); }
  else if (type == "unclaim") { onboarding::handleUnclaim(doc); sendHello(); }
  else if (type == "hello_ack") { if (doc["ota_ws"].is<bool>()) ota::wsSupport(doc["ota_ws"].as<bool>()); }
  else if (type == "ota_available" || type == "ota_none") ota::wsReply(doc);
  else if (type == "cmd") handleCmd(doc);
  else if (type == "event_ack") { if (String(doc["event"] | "") == "factory_reset") { fid::clearResetPending(); LOGI("WS", "RESET_ACKED", "factory_reset event acknowledged"); } }
}

void onEvent(WStype_t type, uint8_t *payload, size_t length) {
  AppHold hold;  // may run inside an unlocked (connecting) ws.loop
  switch (type) {
    case WStype_CONNECTED:
      g_connected = true; g_connectedAt = g_lastRx = millis(); ++g_reconnects;
      g_drop = DropInfo(); ws.resetProbe();
      ota::wsConnected();
      LOGI("WS", "CONNECTED", "%s", g_path.c_str());
      // PR #6: until device_hello is accepted the server refuses every other frame
      // (and a unit not yet in the database is created by it), so it goes first.
      sendHello();
      sendResetEvent();
      offlineq::flushNow();
      offlineq::service(true);  // reconcile offline actions before snapshots (backend ordering)
      snapshot(); sendTelemetry();
      break;
    case WStype_DISCONNECTED:
      if (g_connected) {
        // v12: the library's own reasons are transport failures; never report them as unknown
        if (!g_localClose && !strcmp(g_drop.path, "unknown") && payload && length) {
          if (!strncmp((const char *)payload, "Connection lost", length) || !strncmp((const char *)payload, "TCP connection cleanup", length)) g_drop.path = "transport_lost";
          else if (!strncmp((const char *)payload, "Header response timeout", length)) g_drop.path = "header_timeout";
          else g_drop.path = "lib_reason";
        }
        uint32_t lived = millis() - g_connectedAt;
        // reason: library string ("Connection lost", "Header response timeout"...) if any
        LOGW("WS", "DROP_REASON", "path=%s lib=\"%.*s\" errno=%d tls=%d %s pong_misses=%u lived_ms=%lu since_rx_ms=%lu wifi=%d rssi=%d",
             g_localClose ? "local_close" : g_drop.path, int(payload ? length : 0), payload ? (const char *)payload : "", g_drop.err, g_drop.tls, g_drop.tlsMsg,
             (unsigned)g_drop.pongMisses, (unsigned long)lived, (unsigned long)(millis() - g_lastRx), (int)WiFi.status(), (int)WiFi.RSSI());
        g_drop = DropInfo(); g_localClose = false; ws.resetProbe();
        ota::wsDown();
        if (lived < kQuickCloseMs) {
          if (g_quickCloses < 7) ++g_quickCloses;
          uint32_t retry = kRetryMs << g_quickCloses; if (retry > kRetryMaxMs) retry = kRetryMaxMs;
          ws.setReconnectInterval(retry);
          g_hdrDirty = true; devauth::resync();  // clock skew is the likely cause with HMAC auth
          LOGE("WS", "AUTH_REJECTED", "server closed %lu ms after connect (4401 unauthorized?): check api_key/device_secret/MAC alias; retry in %lu s",
               (unsigned long)lived, (unsigned long)(retry / 1000));
        } else {
          if (g_quickCloses) ws.setReconnectInterval(kRetryMs);
          g_quickCloses = 0;
          LOGW("WS", "DISCONNECTED", "reconnect scheduled");
        }
      }
      g_connected = false; g_hdrDirty = true; break;
    case WStype_TEXT: g_lastRx = millis(); incoming(payload, length); break;
    case WStype_PING: case WStype_PONG: g_lastRx = millis(); break;
    case WStype_ERROR: g_connected = false; g_hdrDirty = true; LOGE("WS", "ERROR", "%.*s", int(length), payload ? (const char *)payload : ""); break;
    default: break;
  }
}
}  // namespace

void addIdentity(JsonDocument &doc) {
  const auto &id = settings::id();
  doc["firmware_version"] = ota::version();
  doc["hardware_model"] = POWERCELL_HARDWARE_MODEL;
  doc["gateway_id"] = id.macColon;   // unchanged (colon form)
  doc["device_id"] = id.deviceId;    // added: matches X-Device-ID
  doc["timestamp"] = unixNow();
  doc["timestamp_ms"] = millis();
}

uint32_t g_maxFrame = 0;
bool send(JsonDocument &doc) {
  if (!g_connected) return false;
  // v13: shared 4 KB scratch (json_scratch) instead of v12's private 8 KB static buffer.
  // A rare larger frame uses a transient exact-size malloc instead of being dropped.
  size_t n = measureJson(doc);
  if (n >= WS_MAX_MESSAGE) { LOGW("WS", "TOO_LARGE", "%u bytes dropped", (unsigned)n); return false; }
  if (n > g_maxFrame + 256 || (!g_maxFrame && n)) { g_maxFrame = n; LOGI("WS", "FRAME_MAX", "bytes=%u", (unsigned)n); }
  if (!wsTake(50)) return false;  // the WS task is mid-loop (connecting)
  bool ok = false;
  if (n < jscratch::kSize && jscratch::take(50)) {
    n = serializeJson(doc, jscratch::buf(), jscratch::kSize);
    ok = ws.sendTXT(reinterpret_cast<uint8_t *>(jscratch::buf()), n);
    jscratch::give();
  } else if (char *b = static_cast<char *>(malloc(n + 1))) {
    n = serializeJson(doc, b, n + 1);
    ok = ws.sendTXT(reinterpret_cast<uint8_t *>(b), n);
    free(b);
  }
  wsGive();
  return ok;
}

namespace { void wsTask(void *); }
void begin() {
  if (!g_app) { g_app = xSemaphoreCreateRecursiveMutex(); g_wsMx = xSemaphoreCreateRecursiveMutex(); }
  String key = settings::apiKey();
  if (key.isEmpty() || key.indexOf('\r') >= 0 || key.indexOf('\n') >= 0) {
    LOGE("WS", "NO_API_KEY", "backend disabled: provision with SET api_key <key> over USB");
    return;
  }
  const auto &id = settings::id();
  char enc[64];
  pc::urlEncode(id.macColon, enc, sizeof enc);
  g_path = String(POWERCELL_WS_PATH) + "?device_id=" + enc;
  g_apiKey = key;
  if (fid::hasSecret()) LOGI("WS", "DEVICE_SECRET", "device_secret set");
  else LOGW("WS", "NO_DEVICE_SECRET", "device_secret missing: requests unsigned");
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
  ws.onEvent(onEvent);
  ws.setReconnectInterval(5000);
  // Client heartbeat: ping every WS_PING_MS, a miss is no pong within
  // WS_PONG_TIMEOUT_MS, disconnect after WS_PING_MISSES misses. Server pings
  // (e.g. Python websockets every 20 s) are answered inside ws.loop().
  ws.enableHeartbeat(WS_PING_MS, WS_PONG_TIMEOUT_MS, WS_PING_MISSES);
  g_inited = true;
  xTaskCreatePinnedToCore(wsTask, "ws", 10240, nullptr, 1, nullptr, 1);
}

namespace {
void serviceOnce(bool locked);
void wsTask(void *) {
  for (;;) {
    bool conn = g_connected;
    if (conn) appLock();
    wsTake(portMAX_DELAY);
    uint32_t t0 = millis();
    serviceOnce(conn);
    uint32_t dt = millis() - t0;
    wsGive();
    if (conn) appUnlock();
    if (dt > LOOP_SLOW_MS) LOGI("WS", "SLOW_PASS", "ws task pass %lu ms (main loop not blocked)", (unsigned long)dt);
    vTaskDelay(pdMS_TO_TICKS(conn ? 2 : 10));
  }
}
}  // namespace
void appLock() { xSemaphoreTakeRecursive(g_app, portMAX_DELAY); }
void appUnlock() { xSemaphoreGiveRecursive(g_app); }
void service() {}  // kept for the API; the work runs in wsTask

namespace {
// v15: signed upgrade headers, rebuilt before every connect attempt (the timestamp and
// nonce are single use). The library keeps a pointer, so setExtraHeaders follows.
void buildHeaders() {
  g_headers = String("API-KEY: ") + g_apiKey + "\r\nX-Device-ID: " + devauth::deviceId();
  devauth::Headers h;
  if (devauth::sign("GET", g_path.c_str(), nullptr, 0, h))
    g_headers += String("\r\nX-Timestamp: ") + h.ts + "\r\nX-Nonce: " + h.nonce + "\r\nX-Signature: " + h.sig;
#if POWERCELL_LEGACY_SECRET_HEADER
  String secret = settings::cloudSecret();
  if (secret.length()) g_headers += "\r\nx-device-secret: " + secret;
#endif
  ws.setExtraHeaders(g_headers.c_str());
  g_hdrDirty = false; g_hdrAt = millis();
}

void serviceOnce(bool locked) {
  if (!g_inited) return;
  if (ota::networkBusy()) return;  // only during an actual firmware install (WS released by pauseForOta)
  if (WiFi.status() != WL_CONNECTED) { g_connected = false; g_wifiUpAt = 0; return; }
  if (!g_wifiUpAt) g_wifiUpAt = millis() | 1;
  if (!net::backendAllowed()) {  // Wi-Fi test / unsaved network: no TLS until /v2/wifi/save
    if (g_started) { g_localClose = true; ws.disconnect(); g_started = false; LOGI("WS", "HOLD", "wifi test in progress; backend waits for save"); }
    g_connected = false; return;
  }
  uint32_t now = millis();
  if (!unixNow()) {
    // mbedTLS here (2.28, MBEDTLS_HAVE_TIME_DATE) does check certificate dates; before NTP
    // the clock comes from the NVS restore (close enough). Command expiry waits for NTP.
    if (now - g_lastNtpLog >= NTP_LOG_PERIOD_MS) { LOGW("TIME", "NTP_WAIT", "clock not set yet; expires_at checks skipped until synced"); g_lastNtpLog = now; }
  } else if (!g_timeLogged) { g_timeLogged = true; LOGI("TIME", "NTP_OK", "unix=%lu", (unsigned long)unixNow()); { AppHold h; onboarding::clockSynced(); } }
  // Signing needs NTP/phone time: give SNTP up to 30 s after Wi-Fi, then fall back to
  // the legacy header so a unit behind a broken NTP path still reaches the server.
  if (!g_started && !devauth::clockTrusted() && fid::hasSecret() && millis() - g_wifiUpAt < 30000) return;
  if (!g_connected && (g_hdrDirty || millis() - g_hdrAt > 20000)) buildHeaders();
  if (!g_started) {
#if POWERCELL_WS_TLS
    ws.beginSslWithCA(POWERCELL_WS_HOST, POWERCELL_WS_PORT, g_path.c_str(), POWERCELL_CA_CERT, "arduino");
#else
    ws.begin(POWERCELL_WS_HOST, POWERCELL_WS_PORT, g_path.c_str(), "arduino");
#endif
    g_started = true;
    LOGI("WS", "CONNECTING", "%s://%s:%u%s", POWERCELL_WS_TLS ? "wss" : "ws", POWERCELL_WS_HOST, POWERCELL_WS_PORT, g_path.c_str());
  }
  if (!g_connected && ota::tlsBusy()) return;  // one TLS session at a time: no WS (re)handshake during an OTA check
#if POWERCELL_WS_TLS
  if (!g_connected && !heapguard::tlsAllowed(g_tlsBackoff, "ws")) return;  // ws.loop() would start the handshake
#endif
  ws.loop();
  if (!g_connected || !locked) return;  // just connected: the next (locked) pass sends
  if (onboarding::helloDue() && millis() - g_connectedAt > 1000) sendHello();
  if (now - g_lastTelemetry >= (powerguard::lowPower() ? TELEMETRY_LOW_POWER_MS : TELEMETRY_PERIOD_MS) || (g_telemetryNow && now - g_lastTelemetry >= 250)) { g_telemetryNow = false; sendTelemetry(); }
  if (now - g_lastSnapshot >= SNAPSHOT_PERIOD_MS) snapshot();
  servicePending();
}
}  // namespace

void requestTelemetry() { g_telemetryNow = true; }

bool sendOtaStatus(const char *status, const char *version) {
  if (!connected()) return false;
  JsonDocument doc;
  doc["type"] = "ota_status";
  addIdentity(doc);
  doc["status"] = status;
  doc["version"] = version;
  doc["from_version"] = POWERCELL_FIRMWARE_VERSION;
  return send(doc);
}

// OTA check over the open WS (contract: /workspace/esp32_review/ota_over_ws.md)
bool sendOtaCheck(bool healthy, const char *lastStatus, const char *lastError) {
  if (!connected()) return false;
  JsonDocument doc;
  doc["type"] = "ota_check";
  addIdentity(doc);
  doc["fw"] = POWERCELL_FIRMWARE_VERSION;
  doc["kind"] = POWERCELL_FIRMWARE_KIND;
  doc["board"] = POWERCELL_HARDWARE_MODEL;
  doc["healthy"] = healthy;
  doc["last_status"] = lastStatus;
  doc["last_error"] = lastError;
  return send(doc);
}

bool connected() { return g_inited && g_connected; }
uint32_t connectedForMs() { return connected() ? millis() - g_connectedAt : 0; }
bool healthy() { return connected() && millis() - g_connectedAt > 2000; }
void publishState() {  // also called from the async web server task
  if (!g_connected || xSemaphoreTakeRecursive(g_app, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (g_connected) snapshot();
  appUnlock();
}
uint32_t reconnects() { return g_reconnects; }

bool pauseForOta() {
  for (auto &p : pending) if (p.active) return false;  // finish relay commands first
  if (!wsTake(0)) return false;  // WS task mid-connect: retried by the OTA gate
  if (g_started) { g_localClose = true; ws.disconnect(); }
  wsGive();
  g_connected = false;
  g_started = false;
  return true;
}
}  // namespace backend
