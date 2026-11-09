#include "prefs_safe.h"
#include "onboarding.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>
#include "local_auth.h"
#include "log.h"
#include "net.h"
#include "onb_core.h"
#include "settings.h"
#include "sha256.h"
#include "clock.h"
#include "factory_id.h"

namespace onboarding {
namespace {
using namespace pc::onb;
constexpr const char *NS = "pc_onb";
constexpr const char *NS_FACTORY_V14 = "pc_factory";  // v14 label passcode; migrated away in v15

SemaphoreHandle_t g_mx = nullptr;
struct Lock {
  Lock() { xSemaphoreTakeRecursive(g_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(g_mx); }
};

Verifier g_cur;
bool g_hasCur = false, g_isDefault = false;
uint32_t g_changedAt = 0;
PairStore g_pairs;
Sessions g_sessions;
Limiter g_global;
struct IpLimit { uint32_t ip; uint32_t lastUse; Limiter l; } g_ipl[8] = {};
String g_owner, g_pending;  // pending passcode_changed origin
bool g_claimActive = false, g_claimIssuedBoot = false;  // issued this boot, no claim_result yet
char g_claimHash[65] = {0};
uint32_t g_claimDeadline = 0;
volatile bool g_helloDirty = true;
struct Ack { String rid, status, error, fp; } g_acks[8];
int g_ackNext = 0;
uint32_t g_rebootAt = 0;

uint32_t unixNow() { time_t t = time(nullptr); return t >= 1700000000 ? uint32_t(t) : 0; }
String randomToken() { uint8_t r[24]; esp_fill_random(r, sizeof r); char t[49]; tokenFromRandom(r, t); return t; }

bool putStr(const char *ns, const char *k, const String &v) {
  Preferences p; if (!p.begin(ns, false)) return false;
  bool ok = v.length() ? p.putString(k, v) == v.length() : (p.isKey(k) ? p.remove(k) : true);
  p.end(); return ok;
}
bool saveVerifier() {
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = true;
  if (g_hasCur) { char h[97]; verifierHex(g_cur, h); ok = p.putString("pass_v", h) == 96; }
  else if (p.isKey("pass_v")) p.remove("pass_v");
  ok = ok && p.putBool("is_def", g_isDefault) && (p.putUInt("chg_at", g_changedAt) || g_changedAt == 0);
  p.end(); return ok;
}
bool savePairs() { Preferences p; if (!p.begin(NS, false)) return false; bool ok = p.putBytes("pairs", &g_pairs, sizeof g_pairs) == sizeof g_pairs; p.end(); return ok; }
void saveLimiter() { Preferences p; if (p.begin(NS, false)) { p.putUShort("lim_f", g_global.fails); p.end(); } }
void setPending(const char *origin) { g_pending = origin; putStr(NS, "pend", g_pending); g_helloDirty = true; }

IpLimit &ipSlot(uint32_t ip) {
  IpLimit *lru = &g_ipl[0];
  for (auto &x : g_ipl) { if (x.ip == ip) { x.lastUse = millis(); return x; } if (x.lastUse < lru->lastUse) lru = &x; }
  *lru = IpLimit{}; lru->ip = ip; lru->lastUse = millis();
  return *lru;
}
}  // namespace

void begin() {
  g_mx = xSemaphoreCreateRecursiveMutex();
  pc::g_pbkdf2Yield = [] { vTaskDelay(1); };  // let the main loop run during the ~0.7 s hash
  Lock l;
  Preferences p;
  if (prefsOpenRO(p, NS)) {
    g_hasCur = verifierFromHex(prefsStr(p, "pass_v", "").c_str(), g_cur);
    g_isDefault = p.getBool("is_def", false);
    g_changedAt = p.getUInt("chg_at", 0);
    g_owner = prefsStr(p, "owner", "");
    g_pending = prefsStr(p, "pend", "");
    if (prefsLen(p, "pairs") != sizeof g_pairs || p.getBytes("pairs", &g_pairs, sizeof g_pairs) != sizeof g_pairs) g_pairs.clear();
    g_global.fails = p.getUShort("lim_f", 0);
    p.end();
  } else g_pairs.clear();
  for (auto &x : g_pairs.p) { x.id[kPhoneIdMax] = 0; x.name[kPhoneNameMax] = 0; }
  if (uint32_t s = Limiter::lockSeconds(g_global.fails)) g_global.lockedUntilMs = millis() + s * 1000;  // survives reboots

  // v15: no factory passcode. A v14 unit still on its label passcode drops it, so the
  // customer sets the first passcode in app setup (/v2/setup/passcode over the hotspot).
  if (nvsNsExists(NS_FACTORY_V14)) {
    if (g_hasCur && g_isDefault) { g_hasCur = false; g_isDefault = false; saveVerifier(); LOGW("ONB", "LABEL_PASSCODE_REMOVED", "unit was on the v14 label passcode; set one in app setup"); }
    if (p.begin(NS_FACTORY_V14, false)) { p.clear(); p.end(); }
    settings::setString("label_pc", "");
  }
  if (g_isDefault) { g_isDefault = false; saveVerifier(); }
  LOGI("ONB", "STATE", "passcode=%s default=%d claimed=%d phones=%u fingerprint=%s",
       g_hasCur ? "set" : "unset(app setup)", g_isDefault, g_owner.length() > 0, (unsigned)g_pairs.count(), fingerprint().c_str());
}

void service() {
  if (g_rebootAt && (int32_t)(millis() - g_rebootAt) >= 0) { LOGW("ONB", "REBOOT", "factory reset applied"); delay(100); ESP.restart(); }
}

String displayId() { return settings::id().gatewayName; }
bool passcodeSet() { Lock l; return g_hasCur; }
bool passcodeDefault() { Lock l; return g_isDefault; }
bool claimed() { Lock l; return g_owner.length() > 0; }
String fingerprint() {
  Verifier v; { Lock l; if (!g_hasCur) return ""; v = g_cur; }
  String secret = settings::deviceSecret();
  if (secret.isEmpty()) return "";
  char fp[33]; pc::onb::fingerprint(secret.c_str(), v, fp);
  return fp;
}

Caller authorize(const String &h) {
  Caller c;
  if (!h.startsWith("Bearer ") || h.length() > 7 + 64) return c;
  String t = h.substring(7); t.trim();
  Lock l;
  int s = g_sessions.find(t.c_str(), millis());
  if (s >= 0) {
    c.auth = Auth::Session; c.phoneId = g_sessions.s[s].phone;
    for (auto &x : g_pairs.p) if (x.used && c.phoneId == x.id) c.phoneName = x.name;
    return c;
  }
  int p = g_pairs.find(t.c_str());
  if (p >= 0) { c.auth = Auth::Lan; c.phoneId = g_pairs.p[p].id; c.phoneName = g_pairs.p[p].name; g_pairs.p[p].lastSeen = unixNow(); }
  return c;
}

bool locked(uint32_t ip, uint32_t *ra) {
  Lock l;
  uint32_t now = millis(), a = 0, b = 0;
  bool g = g_global.locked(now, &a), i = ipSlot(ip).l.locked(now, &b);
  if (ra) *ra = a > b ? a : b;
  return g || i;
}
void authFailed(uint32_t ip) {
  Lock l;
  uint32_t now = millis();
  g_global.fail(now); ipSlot(ip).l.fail(now); saveLimiter();
  LOGW("ONB", "AUTH_FAIL", "ip=%s failures=%u", IPAddress(ip).toString().c_str(), g_global.fails);
}
void authOk(uint32_t ip) {
  Lock l;
  ipSlot(ip).l.success();
  if (g_global.fails) { g_global.success(); saveLimiter(); }
}

bool matchesDeviceId(const String &id) {
  const auto &d = settings::id();
  return id.length() && (id.equalsIgnoreCase(d.gatewayName) || id.equalsIgnoreCase(d.apSsid) || pc::sameDeviceId(id.c_str(), d.deviceId));
}
bool verifyPasscode(const String &p) {
  Verifier v; bool has;
  { Lock l; has = g_hasCur; v = g_cur; }
  // v15: no label passcode and no admin-token fallback. Until the customer sets one in
  // app setup there is nothing to log in with (use /v2/setup/passcode on the hotspot).
  return has && checkPasscode(p.c_str(), v);
}

bool setupPasscode(const String &code, const String &phoneId, const String &phoneName, String &session, String &lan) {
  if (!validPasscode(code.c_str())) return false;
  { Lock l; if (g_hasCur) return false; }
  uint8_t salt[16]; esp_fill_random(salt, sizeof salt);
  Verifier v; makeVerifier(code.c_str(), salt, v);
  Lock l;
  if (g_hasCur) return false;  // raced with another phone
  g_cur = v; g_hasCur = true; g_isDefault = false; g_changedAt = unixNow();
  if (!saveVerifier()) { g_hasCur = false; return false; }
  g_sessions.clear(); g_pairs.clear();
  session = randomToken(); lan = randomToken();
  g_sessions.issue(session.c_str(), millis(), phoneId.c_str());
  g_pairs.issue(phoneId.c_str(), phoneName.c_str(), lan.c_str(), unixNow());
  savePairs();
  setPending("setup");
  LOGI("ONB", "PASSCODE_SETUP", "first passcode set by phone=%s", phoneId.c_str());
  return true;
}

bool openSession(const String &phoneId, const String &phoneName, String &session, String &lan) {
  session = randomToken(); lan = randomToken();
  Lock l;
  g_sessions.issue(session.c_str(), millis(), phoneId.c_str());
  g_pairs.issue(phoneId.c_str(), phoneName.c_str(), lan.c_str(), unixNow());
  return savePairs();
}
String pair(const String &phoneId, const String &phoneName) {
  String t = randomToken();
  Lock l;
  g_pairs.issue(phoneId.c_str(), phoneName.c_str(), t.c_str(), unixNow());
  return savePairs() ? t : String();
}
void pairsJson(JsonArray out) {
  Lock l;
  for (auto &x : g_pairs.p) {
    if (!x.used) continue;
    JsonObject o = out.add<JsonObject>();
    o["phone_id"] = x.id; o["phone_name"] = x.name; o["created_at"] = x.created; o["last_seen"] = x.lastSeen;
  }
}
bool unpair(const String &phoneId) { Lock l; bool r = g_pairs.remove(phoneId.c_str()); if (r) savePairs(); return r; }

bool setPasscode(const String &code, const char *origin, const Caller *caller, String &lan) {
  if (!validPasscode(code.c_str())) return false;
  uint8_t salt[16]; esp_fill_random(salt, sizeof salt);
  Verifier v; makeVerifier(code.c_str(), salt, v);  // ~0.2 s, outside the lock
  Lock l;
  g_cur = v; g_hasCur = true; g_isDefault = false; g_changedAt = unixNow();
  if (!saveVerifier()) return false;
  g_sessions.clear();
  g_pairs.clear();  // every other phone loses LAN access
  lan = "";
  if (caller && caller->phoneId.length()) { lan = randomToken(); g_pairs.issue(caller->phoneId.c_str(), caller->phoneName.c_str(), lan.c_str(), unixNow()); }
  savePairs();
  setPending(origin);
  LOGI("ONB", "PASSCODE_CHANGED", "origin=%s fingerprint=%s", origin, fingerprint().c_str());
  return true;
}

String newClaimCode() {
  uint8_t r[5]; esp_fill_random(r, sizeof r);
  char code[9]; claimCodeFromRandom(r, code);
  Lock l;
  claimHash(settings::id().deviceId, code, g_claimHash);
  g_claimActive = true; g_claimIssuedBoot = true; g_claimDeadline = millis() + kClaimTtlS * 1000;
  g_helloDirty = true;  // report the hash right away when online
  LOGI("ONB", "CLAIM_CODE", "issued, hash=%.12s... ttl=%us", g_claimHash, (unsigned)kClaimTtlS);
  return code;
}

void factoryReset(const char *source, const String &cmdId, uint32_t issuedAt) {
  // v15 (Caleb 2026-10-23): back to as-bought. Wipes owner, passcode, phones, sessions,
  // claim, Wi-Fi/router, net_mode, local token, user hotspot password, tz, sockets, socket
  // settings and the offline queue. Keeps the factory identity (serial, secret, label
  // hotspot password) and backend provisioning (api_key, ota_url). The server is told on
  // the next connect (reset event, reset_pending until event_ack).
  {
    Lock l;
    g_owner = ""; g_hasCur = false; g_isDefault = false; g_pending = "";
    g_pairs.clear(); g_sessions.clear(); g_claimActive = false; g_claimIssuedBoot = false;
    g_global.success();
    Preferences p;
    const char *spaces[] = {NS, "pc_offline", "registry", "forgotten", "sock_cfg", "sock_prio"};
    for (auto ns : spaces) if (nvsNsExists(ns) && p.begin(ns, false)) { p.clear(); p.end(); }
    settings::wipeUser();
    clock_::clearUserSettings();
    uint32_t now = unixNow();
    fid::markReset(source, cmdId, now, issuedAt > now ? issuedAt : now);
    g_helloDirty = true;
    g_rebootAt = millis() + 1500; if (!g_rebootAt) g_rebootAt = 1;
  }
  LOGW("ONB", "FACTORY_RESET", "source=%s cmd=%s: user data wiped, factory identity kept; rebooting", source, cmdId.length() ? cmdId.c_str() : "none");
}

String labelLine() {
  if (settings::provisioningLocked()) return "";
  const auto &d = settings::id();
  return String("PWC2;S=") + d.apSsid + ";P=" + settings::apPassword() + ";D=" + d.gatewayName;
}

void buildHello(JsonDocument &doc) {
  String fp = fingerprint();
  Lock l;
  doc["type"] = "device_hello";
  doc["onboarding_version"] = 2;
  doc["display_id"] = displayId();
  JsonObject p = doc["passcode"].to<JsonObject>();
  if (fp.length()) p["fingerprint"] = fp; else p["fingerprint"] = nullptr;
  p["changed_at"] = g_changedAt;
  p["is_default"] = g_isDefault;
  uint32_t now = millis();
  if (g_claimActive && (int32_t)(now - g_claimDeadline) >= 0) g_claimActive = false;
  if (g_claimActive) {
    JsonObject c = doc["claim"].to<JsonObject>();
    c["code_hash"] = g_claimHash;
    uint32_t u = unixNow();
    c["expires_at"] = u ? u + (g_claimDeadline - now) / 1000 : 0;
  } else doc["claim"] = nullptr;
  if (g_owner.length()) doc["owner_id"] = g_owner; else doc["owner_id"] = nullptr;
  if (WiFi.status() == WL_CONNECTED) doc["lan_ip"] = WiFi.localIP().toString(); else doc["lan_ip"] = nullptr;
}
bool helloDue() { return g_helloDirty; }
void helloSent() { g_helloDirty = false; }
void clockSynced() { Lock l; if (g_claimActive) g_helloDirty = true; }

bool takePasscodeChanged(JsonDocument &doc) {
  String fp = fingerprint();
  Lock l;
  if (g_pending.isEmpty() || !g_hasCur) return false;
  doc["type"] = "passcode_changed";
  if (fp.length()) doc["fingerprint"] = fp; else doc["fingerprint"] = nullptr;
  doc["changed_at"] = g_changedAt;
  doc["origin"] = g_pending;
  doc["is_default"] = g_isDefault;
  g_pending = ""; putStr(NS, "pend", "");
  return true;
}

void handleSetPasscode(JsonDocument &in, JsonDocument &ack, bool tls) {
  String rid = in["request_id"] | "";
  String code = in["new_passcode"] | "";
  uint64_t exp = in["expires_at"] | uint64_t(0);
  in["new_passcode"] = "";  // do not keep the plain value around
  ack["type"] = "set_passcode_ack";
  ack["request_id"] = rid;
  Lock l;
  for (auto &a : g_acks) if (rid.length() && a.rid == rid) {  // idempotent: original ack unchanged
    ack["status"] = a.status;
    if (a.error.length()) ack["error"] = a.error; else ack["error"] = nullptr;
    if (a.fp.length()) ack["fingerprint"] = a.fp; else ack["fingerprint"] = nullptr;
    return;
  }
  const char *err = setPasscodeError(rid.c_str(), tls, unixNow(), exp, code.c_str());
  String lan;
  if (!err && !setPasscode(code, "remote", nullptr, lan)) err = "storage_failed";
  code = "";
  String fp = err ? String() : fingerprint();
  ack["status"] = err ? "rejected" : "confirmed";
  if (err) ack["error"] = err; else ack["error"] = nullptr;
  if (fp.length()) ack["fingerprint"] = fp; else ack["fingerprint"] = nullptr;
  if (rid.length() && rid.length() <= 47) {
    g_acks[g_ackNext] = Ack{rid, err ? "rejected" : "confirmed", err ? err : "", fp};
    g_ackNext = (g_ackNext + 1) % 8;
  }
  LOGI("WS", "SET_PASSCODE", "request=%s result=%s", rid.c_str(), err ? err : "confirmed");
}

void handleClaimResult(JsonDocument &in) {
  String st = in["status"] | "", owner = in["owner_id"] | "";
  if (st != "bound" || !validOwnerId(owner.c_str())) { LOGW("WS", "CLAIM_RESULT", "ignored status=%s", st.c_str()); return; }
  Lock l;
  g_owner = owner; putStr(NS, "owner", owner); g_claimIssuedBoot = false;
  g_claimActive = false;
  g_helloDirty = true;
  LOGI("WS", "CLAIM_RESULT", "bound owner=%s", owner.c_str());
}
void handleUnclaim(JsonDocument &in) {
  String reason = in["reason"] | "";
  if (!validUnclaimReason(reason.c_str())) { LOGW("WS", "UNCLAIM", "ignored reason=%s", reason.c_str()); return; }
  Lock l;
  if (g_claimIssuedBoot) { LOGW("WS", "UNCLAIM", "ignored after claim code"); return; }
  g_owner = ""; putStr(NS, "owner", "");
  g_claimActive = false;
  if (unclaimClearsPairs(reason.c_str())) { g_pairs.clear(); savePairs(); g_sessions.clear(); }
  g_helloDirty = true;
  LOGI("WS", "UNCLAIM", "reason=%s phones_cleared=%d", reason.c_str(), unclaimClearsPairs(reason.c_str()));
}
}  // namespace onboarding
