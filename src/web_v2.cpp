#include "backend_ws.h"
#include "socket_cfg.h"
#include "clock.h"
// Local API /v2/* (firmware 2.1 onboarding, spec section 5). JSON only, body <= 1 KB,
// POST for changes (405 otherwise). Auth: "Authorization: Bearer <session|lan_token>".
#include <ESPAsyncWebServer.h>
#include <new>
#include <WiFi.h>
#include "config.h"
#include "log.h"
#include "net.h"
#include "onboarding.h"
#include "ota.h"
#include "settings.h"
#include "factory_id.h"

namespace onboarding {
namespace {
constexpr size_t kMax = 1024;
struct Body { size_t len; bool overflow; char data[kMax + 1]; };

void onBody(AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (r->_tempObject) { free(r->_tempObject); r->_tempObject = nullptr; }
    Body *b = static_cast<Body *>(malloc(sizeof(Body)));
    if (!b) return;
    b->len = 0; b->overflow = total > kMax; r->_tempObject = b;
  }
  Body *b = static_cast<Body *>(r->_tempObject);
  if (!b || b->overflow) return;
  if (b->len + len > kMax) { b->overflow = true; return; }
  memcpy(b->data + b->len, data, len); b->len += len; b->data[b->len] = 0;
}
void send(AsyncWebServerRequest *r, int code, JsonDocument &d) {
  // nothrow: a throwing new in the async_tcp task aborts the whole unit (bad_alloc
  // under TLS heap pressure). Out of memory: drop the connection, the app retries.
  AsyncResponseStream *s = new (std::nothrow) AsyncResponseStream("application/json", 256);
  if (!s) { if (r->client()) r->client()->close(); return; }
  s->setCode(code); s->addHeader("Cache-Control", "no-store");
  serializeJson(d, *s); r->send(s);
}
void err(AsyncWebServerRequest *r, int code, const char *e, uint32_t retryAfter = 0) {
  JsonDocument d; d["error"] = e;
  if (retryAfter) d["retry_after"] = retryAfter;
  send(r, code, d);
}
void okTrue(AsyncWebServerRequest *r) { JsonDocument d; d["ok"] = true; send(r, 200, d); }
uint32_t ipOf(AsyncWebServerRequest *r) { return r->client() ? uint32_t(r->client()->remoteIP()) : 0; }
String str(JsonDocument &d, const char *k) { return d[k].is<const char *>() ? d[k].as<String>() : d[k].is<long>() ? String(d[k].as<long>()) : String(); }

using H = std::function<void(AsyncWebServerRequest *, JsonDocument &, const Caller &)>;
// auth: true = session or LAN token required.
void route(AsyncWebServer &s, const char *path, WebRequestMethod m, bool auth, H h) {
  s.on(path, HTTP_ANY, [m, auth, h](AsyncWebServerRequest *r) {
    if (r->method() != m) {
      AsyncWebServerResponse *res = new (std::nothrow) AsyncBasicResponse(405, "application/json", "{\"error\":\"method_not_allowed\"}");
      if (!res) { if (r->client()) r->client()->close(); return; }
      res->addHeader("Allow", m == HTTP_POST ? "POST" : "GET");
      r->send(res); return;
    }
    JsonDocument doc;
    Body *b = static_cast<Body *>(r->_tempObject);
    if (b && b->overflow) return err(r, 413, "body_too_large");
    if (b && b->len && (deserializeJson(doc, b->data, b->len) || !doc.is<JsonObject>())) return err(r, 400, "invalid_json");
    Caller c;
    if (auth) {
      c = authorize(r->hasHeader("Authorization") ? r->header("Authorization") : String());
      if (c.auth == Auth::None) return err(r, 401, "unauthorized");
    }
    h(r, doc, c);
  }, nullptr, onBody);
}

// Shared limiter (spec section 5): /v2/login, /v2/passcode (old value), /v2/factory_reset.
bool checkSecret(AsyncWebServerRequest *r, const String &secret, bool (*verify)(const String &)) {
  uint32_t ip = ipOf(r), ra = 0;
  if (locked(ip, &ra)) { err(r, 429, "locked", ra); return false; }
  if (!verify(secret)) {
    authFailed(ip);
    locked(ip, &ra);
    err(r, 401, "bad_credentials", ra);
    return false;
  }
  authOk(ip);
  return true;
}
bool g_matchId = false;
String g_loginId;
bool verifyLogin(const String &p) { return g_matchId && verifyPasscode(p); }
}  // namespace

void registerRoutes(AsyncWebServer &s) {
  route(s, "/v2/setup/info", HTTP_GET, false, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    JsonDocument d;
    d["device_id"] = displayId();
    d["model"] = POWERCELL_HARDWARE_MODEL;
    d["fw"] = ota::version();
    d["state"] = settings::wifiSsid().length() ? "configured" : "unconfigured";
    d["claimed"] = claimed();
    d["passcode_default"] = passcodeDefault();
    d["passcode_set"] = passcodeSet();  // v15: false until app setup (POST /v2/setup/passcode)
    d["hotspot"] = net::apActive();
    d["onboarding_version"] = 2;
    send(r, 200, d);
  });
  // v15: first passcode, no factory default. Only while none is set and only from a phone
  // on the hotspot (it knows the label Wi-Fi password). Returns a session like /v2/login.
  route(s, "/v2/setup/passcode", HTTP_POST, false, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    String code = str(doc, "passcode"), phone = str(doc, "phone_id"), name = str(doc, "phone_name");
    if (!phone.length() || phone.length() > 40 || name.length() > 32 || !code.length() || code.length() > 64) return err(r, 400, "invalid_request");
    if (passcodeSet()) return err(r, 409, "passcode_already_set");
    bool onAp = r->client() && WiFi.softAPIP() != IPAddress(0, 0, 0, 0) && r->client()->localIP() == WiFi.softAPIP();
    if (!onAp) return err(r, 403, "hotspot_required");
    uint32_t ra = 0;
    if (locked(ipOf(r), &ra)) return err(r, 429, "locked", ra);
    String session, lan;
    if (!setupPasscode(code, phone, name, session, lan)) return err(r, passcodeSet() ? 409 : 422, passcodeSet() ? "passcode_already_set" : "invalid_passcode");
    JsonDocument d;
    d["session"] = session; d["expires_in"] = 1800; d["lan_token"] = lan; d["passcode_default"] = false;
    d["fingerprint"] = fingerprint(); if (fingerprint().isEmpty()) d["fingerprint"] = nullptr;
    send(r, 200, d);
  });
  route(s, "/v2/login", HTTP_POST, false, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    String id = str(doc, "device_id"), pc_ = str(doc, "passcode"), phone = str(doc, "phone_id"), name = str(doc, "phone_name");
    if (!phone.length() || phone.length() > 40 || name.length() > 32 || !pc_.length() || pc_.length() > 64) return err(r, 400, "invalid_request");
    g_matchId = matchesDeviceId(id);  // AsyncTCP runs handlers one at a time
    bool good = checkSecret(r, pc_, verifyLogin);
    g_matchId = false;
    if (!good) return;
    String session, lan;
    if (!openSession(phone, name, session, lan)) return err(r, 500, "storage_failed");
    JsonDocument d;
    d["session"] = session; d["expires_in"] = 1800; d["lan_token"] = lan; d["passcode_default"] = passcodeDefault();
    send(r, 200, d);
    LOGI("ONB", "LOGIN", "phone=%s", phone.c_str());
  });
  route(s, "/v2/pair", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &c) {
    String phone = str(doc, "phone_id"), name = str(doc, "phone_name");
    if (!phone.length()) phone = c.phoneId;
    if (!phone.length() || phone.length() > 40 || name.length() > 32) return err(r, 400, "invalid_request");
    String t = pair(phone, name.length() ? name : c.phoneName);
    if (t.isEmpty()) return err(r, 500, "storage_failed");
    JsonDocument d; d["lan_token"] = t; send(r, 200, d);
  });
  route(s, "/v2/pairs", HTTP_GET, true, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    JsonDocument d; pairsJson(d["phones"].to<JsonArray>()); send(r, 200, d);
  });
  // POST /v2/time {epoch, tz_offset_min}: session or LAN token. See time_policy.h.
  route(s, "/v2/time", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    if (!doc["epoch"].is<long long>()) return err(r, 400, "invalid_request");
    bool hasTz = doc["tz_offset_min"].is<long>();
    long tz = hasTz ? doc["tz_offset_min"].as<long>() : 0;
    if (hasTz && !pc::clk::validTz(tz)) return err(r, 400, "invalid_tz_offset");
    pc::clk::Verdict v = clock_::setFromPhone(doc["epoch"].as<long long>(), hasTz, tz);
    if (v == pc::clk::Verdict::Invalid) return err(r, 400, "invalid_epoch");
    JsonDocument d; d["ok"] = true; d["applied"] = v == pc::clk::Verdict::Accept;
    if (v != pc::clk::Verdict::Accept) d["reason"] = pc::clk::verdictName(v);
    clock_::toJson(d.as<JsonObject>()); send(r, 200, d);
  });
  // AcePlug socket config over the LAN: same body as the WS commands
  // ({"type":"SET_LIMIT|SET_CONFIG|SET_OVERRIDE","socket":"<id>",...}).
  route(s, "/v2/socket-config", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    String sock;
    const char *e = socketcfg::apply(doc.as<JsonObjectConst>(), nullptr, String(), sock);
    if (e) return err(r, !strcmp(e, "unknown_socket") ? 404 : !strcmp(e, "storage_failed") ? 500 : 400, e);
    JsonDocument d; d["ok"] = true; d["socket"] = sock;
    socketcfg::stateJson(sock, d["state"].to<JsonObject>()); send(r, 200, d);
  });
  route(s, "/v2/unpair", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    String phone = str(doc, "phone_id");
    if (!phone.length()) return err(r, 400, "invalid_request");
    if (!unpair(phone)) return err(r, 404, "unknown_phone");
    okTrue(r);
  });
  route(s, "/v2/passcode", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &c) {
    String oldp = str(doc, "old"), newp = str(doc, "new");
    if (!newp.length() || newp.length() > 64 || oldp.length() > 64) return err(r, 400, "invalid_request");
    if (!checkSecret(r, oldp, verifyPasscode)) return;
    String lan;
    if (!setPasscode(newp, "local", &c, lan)) return err(r, 422, "invalid_passcode");  // 6 digits, not all one digit
    JsonDocument d; d["ok"] = true; d["fingerprint"] = fingerprint(); d["lan_token"] = lan;
    if (fingerprint().isEmpty()) d["fingerprint"] = nullptr;
    send(r, 200, d);
  });
  route(s, "/v2/ap_password", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    String p = str(doc, "new");
    if (p.length() < 8 || p.length() > 63) return err(r, 400, "invalid_password");
    for (char ch : p) if (ch < 0x20 || ch > 0x7e) return err(r, 400, "invalid_password");
    if (!settings::setString("ap_pass", p)) return err(r, 500, "storage_failed");
    net::restartAp(5000);
    JsonDocument d; d["ok"] = true; d["applies"] = "after_reconnect"; send(r, 200, d);
  });
  route(s, "/v2/wifi/scan", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    JsonDocument d;
    bool fresh = net::scan(d["networks"].to<JsonArray>());
    d["scanning"] = !fresh;  // results older than 30 s: a new scan runs; poll again in ~3 s
    send(r, 200, d);
  });
  route(s, "/v2/wifi/test", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    String ssid = str(doc, "ssid"), pass = str(doc, "password");
    bool fiveG = doc["phone_sees_5ghz"] | false;
    String id;
    if (!net::startTest(ssid, pass, fiveG, id)) return err(r, ssid.length() ? 409 : 400, ssid.length() ? "test_busy_or_invalid" : "invalid_request");
    JsonDocument d; d["test_id"] = id; send(r, 202, d);
  });
  route(s, "/v2/wifi/status", HTTP_GET, true, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    JsonDocument d; net::testStatus(d); send(r, 200, d);
  });
  route(s, "/v2/wifi/save", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    String ip;
    if (!net::saveTested(ip)) return err(r, 409, "no_successful_test");
    JsonDocument d; d["ok"] = true; d["ip"] = ip; d["hotspot_off_in"] = 120; send(r, 200, d);
  });
  // v14: hotspot-only network mode (session required; LAN token not accepted)
  route(s, "/v2/network_mode", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &c) {
    if (c.auth != Auth::Session) return err(r, 403, "session_required");
    pc::onb::NetMode m;
    String ms = str(doc, "mode");
    if (!pc::onb::parseNetMode(ms.c_str(), m)) return err(r, 400, "invalid_mode");
    if (!net::setNetworkMode(m)) return err(r, 500, "storage_failed");
    JsonDocument d; d["ok"] = true; d["mode"] = pc::onb::netModeName(m); send(r, 200, d);
    LOGI("ONB", "NETWORK_MODE", "mode=%s phone=%s", pc::onb::netModeName(m), c.phoneId.c_str());
  });
  route(s, "/v2/status", HTTP_GET, true, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    JsonDocument d;
    d["device_id"] = displayId();
    d["fw"] = ota::version();
    d["network_mode"] = pc::onb::netModeName(net::networkMode());
    d["cloud_linked"] = claimed();
    d["wifi_state"] = net::stateName();
    d["hotspot"] = net::apActive();
    d["sta_connected"] = net::staConnected();
    d["cloud_connected"] = backend::connected();
    d["payment_locked"] = fid::paymentLocked();
    d["payment_lock_reason"] = fid::paymentLocked() ? fid::paymentLockReason() : "";
    send(r, 200, d);
  });
  route(s, "/v2/claim_code", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &, const Caller &) {
    JsonDocument d; d["claim_code"] = newClaimCode(); d["expires_in"] = 900; send(r, 200, d);
  });
  route(s, "/v2/factory_reset", HTTP_POST, true, [](AsyncWebServerRequest *r, JsonDocument &doc, const Caller &) {
    if (!checkSecret(r, str(doc, "passcode"), verifyPasscode)) return;
    okTrue(r);
    factoryReset("app");
  });
}
}  // namespace onboarding
