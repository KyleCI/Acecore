#include "json_scratch.h"
#include "power_guard.h"
#include "web_local.h"
#include <ArduinoJson.h>
#include <new>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include "backend_ws.h"
#include "battery.h"
#include "config.h"
#include "gateway.h"
#include "factory_id.h"
#include "local_auth.h"
#include "log.h"
#include "net.h"
#include "offline_queue.h"
#include "onboarding.h"
#include "ota.h"
#include "settings.h"
#include "sockets/sockets.h"
#include "stm32_link.h"
#include "telemetry.h"
#include "events.h"

namespace web {
namespace {
AsyncWebServer server(80);
AsyncEventSource events("/events");
SemaphoreHandle_t g_lock = nullptr, g_bufLock = nullptr;
String g_label;                  // setup token from the label (AP only)
String g_token;                  // backend local_access_token (cached; refreshed on change)
volatile bool g_tokenDirty = true;
volatile int g_pendingMode = -1;
volatile bool g_pendingFaultReset = false;
volatile bool g_restart = false;
volatile bool g_stateChanged = false;
uint32_t g_restartAt = 0;

// ---- notifications / snapshot (ported from 1.0.4 offline_battery_notifications) ----
pc::BatteryAlertPolicy g_policy;
uint64_t g_seq = 0, g_batSeq = 0, g_unavailSeq = 0;
bool g_seqPersistent = false;
String g_snapshot = "{}", g_batJson = "{}", g_unavailJson = "{}";
constexpr size_t kSnapBuf = 3072, kAlertBuf = 512;  // v13: String reserve 4096 -> 3072
uint32_t g_lastSample = 0, g_lastHeartbeat = 0;
bool g_sampled = false;

enum class Cred : uint8_t { None, Setup, Local };

const char kSetup[] PROGMEM = R"html(<!doctype html><html><head><meta name=viewport content="width=device-width">
<title>Powercell setup</title><style>body{font-family:sans-serif;max-width:420px;margin:2em auto}input,button{width:100%;padding:.6em;margin:.3em 0}</style></head>
<body><h2>Powercell setup</h2><p>Connect the unit to Wi-Fi. Use the setup token printed on the label, or the device login from the Powercell app.</p>
<form id=f><input name=ssid placeholder="Wi-Fi name" required><input name=pass type=password placeholder="Wi-Fi password">
<input id=t type=password placeholder="Setup token or device login" required><button>Save and restart</button></form><pre id=o></pre>
<script>f.onsubmit=async e=>{e.preventDefault();const b=Object.fromEntries(new FormData(f));const r=await fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Basic '+btoa('powercell:'+t.value)},body:JSON.stringify(b)});o.textContent=r.status+' '+await r.text()}</script>
</body></html>)html";

const char kDash[] PROGMEM = R"html(<!doctype html><html><head><meta name=viewport content="width=device-width">
<title>Powercell</title><style>body{font-family:sans-serif;max-width:480px;margin:2em auto}button{padding:.6em 1.2em;margin:.3em}</style></head>
<body><h2>Powercell</h2><pre id=s>loading...</pre><button onclick="st('on')">Output on</button><button onclick="st('off')">Output off</button>
<script>const h={'Content-Type':'application/json'};async function ld(){const r=await fetch('/data',{cache:'no-store'});s.textContent=JSON.stringify(await r.json(),null,1)}
async function st(v){await fetch('/state',{method:'POST',headers:h,body:JSON.stringify({state:v})});ld()}ld();setInterval(ld,5000)</script>
</body></html>)html";

void refreshToken() {
  if (!g_tokenDirty) return;
  String t = settings::localToken();
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE) { g_token = t; g_tokenDirty = false; xSemaphoreGive(g_lock); }
}
String token() {
  String t;
  if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE) { t = g_token; xSemaphoreGive(g_lock); }
  return t;
}

bool fromSetupAp(AsyncWebServerRequest *r) {
  if (!net::apActive() || !r->client()) return false;
  return r->client()->localIP() == WiFi.softAPIP();
}

Cred credFromSecret(AsyncWebServerRequest *r, const char *user, const char *secret) {
  if (!user || strcmp(user, pc::kLocalUser) != 0 || !secret) return Cred::None;
  String t = token();
  bool local = t.length() && pc::ctEquals(secret, strlen(secret), t.c_str(), t.length());
  bool setup = g_label.length() && pc::ctEquals(secret, strlen(secret), g_label.c_str(), g_label.length());
  if (local) return Cred::Local;
  if (setup && fromSetupAp(r)) return Cred::Setup;
  return Cred::None;
}

Cred credOf(AsyncWebServerRequest *r) {
  if (!r->hasHeader("Authorization")) return Cred::None;
  char u[24], p[pc::kLocalTokenMax + 2];
  if (!pc::parseBasic(r->header("Authorization").c_str(), u, sizeof u, p, sizeof p)) return Cred::None;
  uint32_t ip = r->client() ? uint32_t(r->client()->remoteIP()) : 0;
  if (onboarding::locked(ip, nullptr)) { memset(p, 0, sizeof p); return Cred::None; }  // shared 2.1 limiter
  Cred c = credFromSecret(r, u, p);
  memset(p, 0, sizeof p);
  if (c == Cred::None) onboarding::authFailed(ip);
  return c;
}

// Out of memory for a reply: drop the connection (the app retries) instead of aborting.
void dropOom(AsyncWebServerRequest *r) { if (r->client()) r->client()->close(); }

void json(AsyncWebServerRequest *r, int code, JsonDocument &doc) {
  // nothrow: a throwing new in the async_tcp task aborts the unit under TLS heap pressure.
  AsyncResponseStream *res = new (std::nothrow) AsyncResponseStream("application/json", 256);
  if (!res) { dropOom(r); return; }
  res->setCode(code);
  res->addHeader("Cache-Control", "no-store");
  serializeJson(doc, *res);
  r->send(res);
}
void ok(AsyncWebServerRequest *r, const char *msg = "ok", int code = 200) { JsonDocument d; d["ok"] = true; d["message"] = msg; json(r, code, d); }
void fail(AsyncWebServerRequest *r, int code, const char *err) { JsonDocument d; d["ok"] = false; d["error"] = err; json(r, code, d); }
void unauthorized(AsyncWebServerRequest *r, bool challenge = false) {
  JsonDocument d; d["ok"] = false; d["error"] = "unauthorized";
  AsyncResponseStream *res = new (std::nothrow) AsyncResponseStream("application/json", 256);
  if (!res) { dropOom(r); return; }
  res->setCode(401);
  // Only GET / challenges: a challenge on fetch() would pop a WebView login dialog.
  if (challenge) res->addHeader("WWW-Authenticate", "Basic realm=\"Powercell\", charset=\"UTF-8\"");
  serializeJson(d, *res);
  r->send(res);
}
bool requireLocal(AsyncWebServerRequest *r) {
  if (credOf(r) == Cred::Local) return true;
  unauthorized(r);
  return false;
}

// ---- JSON / form body collection ----
constexpr size_t kMaxBody = 1024;
struct BodyBuf { size_t len; bool overflow; char data[kMaxBody + 1]; };
void onBody(AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (r->_tempObject) { free(r->_tempObject); r->_tempObject = nullptr; }
    BodyBuf *b = static_cast<BodyBuf *>(malloc(sizeof(BodyBuf)));
    if (!b) return;
    b->len = 0; b->overflow = total > kMaxBody;
    r->_tempObject = b;
  }
  BodyBuf *b = static_cast<BodyBuf *>(r->_tempObject);
  if (!b || b->overflow) return;
  if (b->len + len > kMaxBody) { b->overflow = true; return; }
  memcpy(b->data + b->len, data, len); b->len += len; b->data[b->len] = 0;
}
// Returns false (and has answered 400/413) if a body was sent but is not usable.
bool parseBody(AsyncWebServerRequest *r, JsonDocument &doc) {
  BodyBuf *b = static_cast<BodyBuf *>(r->_tempObject);
  if (b && b->overflow) { fail(r, 413, "body too large"); return false; }
  if (b && b->len) {
    if (deserializeJson(doc, b->data, b->len) || !doc.is<JsonObject>()) { fail(r, 400, "invalid JSON body"); return false; }
  }
  return true;
}
String field(AsyncWebServerRequest *r, JsonDocument &doc, const char *name) {
  if (doc[name].is<const char *>()) return doc[name].as<String>();
  if (doc[name].is<long>()) return String(doc[name].as<long>());
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  return "";
}

// POST-only route: other methods 405 (before auth, as specified), then auth, then handler.
using Handler = std::function<void(AsyncWebServerRequest *, JsonDocument &, Cred)>;
void postRoute(const char *path, bool allowSetup, Handler h) {
  server.on(path, HTTP_ANY,
    [allowSetup, h](AsyncWebServerRequest *r) {
      if (r->method() != HTTP_POST) {
        AsyncWebServerResponse *res = new (std::nothrow) AsyncBasicResponse(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
        if (!res) { dropOom(r); return; }
        res->addHeader("Allow", "POST");
        r->send(res);
        return;
      }
      JsonDocument doc;
      if (!parseBody(r, doc)) return;
      Cred c = credOf(r);
      if (c == Cred::None && !strcmp(r->url().c_str(), "/login")) {
        // /login may carry the credential only in the JSON body.
        String u = doc["username"] | "", t = doc["token"] | "";
        if (u.length() && t.length()) c = credFromSecret(r, u.c_str(), t.c_str());
      }
      if (c == Cred::None || (c == Cred::Setup && !allowSetup)) { unauthorized(r); return; }
      h(r, doc, c);
    },
    nullptr, onBody);
}

// ---- snapshot / notifications ----
void reserveBootSequence() {
  Preferences p;
  uint32_t boot = 0;
  if (p.begin("battery_alerts", false)) {  // same namespace as 1.0.4: sequences keep increasing across the upgrade
    boot = p.getUInt("boot", 0) + 1;
    if (boot >= 0x200000 || p.putUInt("boot", boot) == 0) boot = 0;
    p.end();
  }
  g_seqPersistent = boot != 0;
  if (!g_seqPersistent) boot = (esp_random() & 0x1fffff) | 1;
  g_seq = static_cast<uint64_t>(boot) << 32;
  g_batSeq = ++g_seq;
  g_unavailSeq = ++g_seq;
}

void alertJson(JsonObject o, bool availability, const battery::Reading &b) {
  o["collector_id"] = settings::id().deviceId;
  o["sequence"] = availability ? g_unavailSeq : g_batSeq;
  o["sequence_persistent"] = g_seqPersistent;
  o["dummy"] = false;
  if (availability) {
    bool active = !g_policy.available();
    o["id"] = "battery_reading_unavailable";
    o["type"] = "battery_reading_unavailable";
    o["active"] = active;
    o["severity"] = "warning";
    o["sound"] = active ? "warning" : "none";
    o["title"] = active ? "Battery reading unavailable" : "Battery readings restored";
    o["message"] = active ? "No fresh, valid battery reading is available." : "Fresh battery readings are available again.";
    return;
  }
  auto lvl = g_policy.level();
  bool active = lvl != pc::BatteryLevel::None;
  bool critical = lvl == pc::BatteryLevel::Critical || lvl == pc::BatteryLevel::ShutdownRisk;
  o["id"] = "battery_low";
  o["type"] = pc::batteryLevelType(lvl);
  o["active"] = active;
  o["severity"] = critical ? "critical" : active ? "warning" : "info";
  o["sound"] = active ? (critical ? "critical" : "warning") : "none";
  o["title"] = !g_policy.available() ? "Battery reading unavailable" : lvl == pc::BatteryLevel::ShutdownRisk ? "Battery extremely low"
             : lvl == pc::BatteryLevel::Critical ? "Critical battery level" : active ? "Low battery warning" : "Battery level recovered";
  if (g_policy.available() && b.valid) {
    o["battery_percentage"] = b.soc;
    String m = "Battery is at " + String(b.soc) + "%.";
    if (active) m += " Reduce non-essential loads and recharge.";
    o["message"] = m;
  } else {
    o["battery_percentage"] = nullptr;
    o["message"] = "The previous battery alert is no longer current because battery data is unavailable.";
  }
}

void sample(bool force) {
  uint32_t now = millis();
  if (!force && g_sampled && now - g_lastSample < 1000) return;
  g_sampled = true; g_lastSample = now;
  battery::Reading b = battery::read();
  uint8_t ch = g_policy.update(b.valid, b.soc, now);
  if (ch & pc::BatteryAlertPolicy::BatteryChanged) g_batSeq = ++g_seq;
  if (ch & pc::BatteryAlertPolicy::AvailabilityChanged) g_unavailSeq = ++g_seq;

  JsonDocument bat, un, d;
  alertJson(bat.to<JsonObject>(), false, b);
  alertJson(un.to<JsonObject>(), true, b);
  const auto &id = settings::id();
  d["device_id"] = id.deviceId;
  d["mac"] = id.macColon;
  d["gateway_name"] = id.gatewayName;
  d["firmware_version"] = ota::version();
  d["device_state"] = gateway::state() ? "On" : "Off";
  d["state"] = gateway::state() ? "on" : "off";
  d["payment_locked"] = fid::paymentLocked();
  d["payment_lock_reason"] = fid::paymentLocked() ? fid::paymentLockReason() : "";
  d["cloud_connected"] = backend::connected();
  JsonObject bs = d["battery"].to<JsonObject>();
  bs["valid"] = g_policy.available() && b.valid;
  bs["status"] = g_policy.available() ? "available" : "unavailable";
  bs["source"] = b.source;
  if (b.valid) { bs["percentage"] = b.soc; bs["voltage"] = b.voltage; bs["age_ms"] = b.ageMs; }
  else { bs["percentage"] = nullptr; bs["voltage"] = nullptr; bs["age_ms"] = nullptr; }
  JsonDocument m; telemetry::buildMetrics(m);
  d["metrics"] = m;
  JsonArray n = d["notifications"].to<JsonArray>();
  n.add(bat.as<JsonObjectConst>());
  n.add(un.as<JsonObjectConst>());
  ::events::notificationsToJson(n);  // 2.2 events, same id/sequence/title/message/sound shape
  // v12: fixed static buffers + pre-reserved Strings: no per-refresh heap churn
  // buffers guarded by g_bufLock (never held together with AsyncEventSource's lock order
  // inverted: g_lock is released before events.send, as before)
  // v13: the snapshot is serialized into the shared 4 KB scratch (was a private static
  // 4 KB buffer duplicating the reserved g_snapshot String)
  static char bj[kAlertBuf], uj[kAlertBuf];
  if (!jscratch::take(200)) return;
  xSemaphoreTake(g_bufLock, portMAX_DELAY);
  char *data = jscratch::buf();
  serializeJson(d, data, jscratch::kSize); serializeJson(bat, bj, sizeof bj); serializeJson(un, uj, sizeof uj);
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_snapshot = data; g_batJson = bj; g_unavailJson = uj;
  xSemaphoreGive(g_lock);
  if (ch & pc::BatteryAlertPolicy::BatteryChanged) events.send(bj, "notification", (uint32_t)g_batSeq);
  if (ch & pc::BatteryAlertPolicy::AvailabilityChanged) events.send(uj, "notification", (uint32_t)g_unavailSeq);
  if (ch || force || now - g_lastHeartbeat >= 10000) {
    g_lastHeartbeat = now;
    events.send(data, "state");
    events.send("{}", "heartbeat");
  }
  xSemaphoreGive(g_bufLock);
  jscratch::give();
}

void copySnapshot(String &data, String *bat = nullptr, String *un = nullptr) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  data = g_snapshot;
  if (bat) *bat = g_batJson;
  if (un) *un = g_unavailJson;
  xSemaphoreGive(g_lock);
}

void afterGatewayChange(const String &s) {
  if (backend::connected()) backend::publishState();
  else offlineq::queue("gateway_state", "", s.c_str());
  g_stateChanged = true;
}
}  // namespace

void begin() {
  ::events::onNotification([](const char *json, uint32_t seq) { events.send(json, "notification", seq); });
  g_lock = xSemaphoreCreateMutex(); g_bufLock = xSemaphoreCreateMutex();
  g_snapshot.reserve(kSnapBuf); g_batJson.reserve(kAlertBuf); g_unavailJson.reserve(kAlertBuf);  // v12: allocated once at boot
  g_label = settings::localAdminToken();
  refreshToken();
  reserveBootSequence();
  sample(true);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    Cred c = credOf(r);
    if (c == Cred::Local) { r->send(200, "text/html", kDash); return; }
    // No credential yet: only the setup page, and only where setup is possible.
    if (c == Cred::Setup || fromSetupAp(r) || token().isEmpty()) { r->send(200, "text/html", kSetup); return; }
    unauthorized(r, true);
  });
  server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest *r) {
    JsonDocument d; d["ok"] = true; d["firmware_version"] = ota::version(); d["device_id"] = settings::id().deviceId;
    d["local_token_set"] = token().length() > 0;
    json(r, 200, d);
  });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!requireLocal(r)) return;
    String data; copySnapshot(data);
    AsyncWebServerResponse *res = new (std::nothrow) AsyncBasicResponse(200, "application/json", data);
    if (!res) { dropOom(r); return; }
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  events.addMiddleware([](AsyncWebServerRequest *r, ArMiddlewareNext next) {
    if (credOf(r) != Cred::Local) { unauthorized(r); return; }
    next();
  });
  events.onConnect([](AsyncEventSourceClient *client) {
    String data, bat, un;
    copySnapshot(data, &bat, &un);
    client->send("{\"status\":\"connected\"}", "ready", 0, 1000);
    client->send(data.c_str(), "state");
    // Replay inactive records too: a reconnecting app must cancel recovered alerts.
    client->send(bat.c_str(), "notification");
    client->send(un.c_str(), "notification");
  });
  server.addHandler(&events);

  postRoute("/login", true, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred c) {
    // If both body and header are sent they must name the same token.
    String t = doc["token"] | "";
    String u = doc["username"] | pc::kLocalUser;
    if (u != pc::kLocalUser || (t.length() && credFromSecret(r, u.c_str(), t.c_str()) != c)) { unauthorized(r); return; }
    JsonDocument d;
    d["ok"] = true; d["username"] = pc::kLocalUser; d["device_id"] = settings::id().deviceId;
    d["scope"] = c == Cred::Local ? "device" : "setup";
    json(r, 200, d);
  });
  postRoute("/state", false, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    String s = field(r, doc, "state"); s.toLowerCase();
    if (s != "on" && s != "off") return fail(r, 400, "state must be on|off");
    if (fid::paymentLocked()) return fail(r, 403, "payment_locked");
    if (s == "on" && ota::busy()) return fail(r, 409, "refused");
    if (s == "on" && powerguard::latched()) return fail(r, 409, "battery_critical_latched");
    if (s == "on" && !stm32link::online()) return fail(r, 503, "stm32_timeout");  // IO15 stays LOW
    // Applied from loop(): the framed STM32 set_mode blocks until the STM32 ACKs.
    gateway::request(s == "on", "local");
    afterGatewayChange(s);
    JsonDocument d; d["ok"] = true; d["state"] = s; d["applied_via"] = gateway::viaFramed() ? "stm32_set_mode" : "pwr_key";
    json(r, 200, d);
  });
  postRoute("/relay", false, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    String id = field(r, doc, "socket_id"), s = field(r, doc, "state"); s.toLowerCase();
    if (!id.length() || id.length() > 32 || (s != "on" && s != "off")) return fail(r, 400, "socket_id and state=on|off required");
    if (!checkRegistered(id)) return fail(r, 404, "unknown socket");
    if (fid::paymentLocked()) return fail(r, 403, "payment_locked");
    if (s == "on" && powerguard::latched()) return fail(r, 409, "battery_critical_latched");
    if (!sendRelayCommand(id, s == "on")) return fail(r, 409, "socket offline or busy");
    if (!backend::connected()) offlineq::queue("socket_state", id.c_str(), s.c_str());
    g_stateChanged = true;
    ok(r, "queued", 202);
  });
  // {"socket_id":"<id>"}: clears the socket's 3-trips lockout (relay stays off). 202 = queued;
  // poll GET /devices or /data: lockout false + command_type reset_lockout, command_status confirmed.
  postRoute("/reset_lockout", false, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    String id = field(r, doc, "socket_id"); if (!id.length()) id = field(r, doc, "id");
    if (!id.length() || id.length() > 32) return fail(r, 400, "socket_id required");
    if (!checkRegistered(id)) return fail(r, 404, "unknown socket");
    if (!sendResetLockout(id)) return fail(r, 409, "socket offline or busy");
    if (!backend::connected()) offlineq::queue("socket_reset_lockout", id.c_str());
    g_stateChanged = true;
    ok(r, "queued", 202);
  });
  postRoute("/register", false, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    String id = field(r, doc, "socket_id"); if (!id.length()) id = field(r, doc, "id");
    if (!id.length() || id.length() > 32) return fail(r, 400, "socket_id required");
    if (!registerDevice(id)) return fail(r, 404, "socket not found; plug it in and retry");
    ok(r, "pairing", 202);
  });
  postRoute("/unregister", false, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    String id = field(r, doc, "socket_id"); if (!id.length()) id = field(r, doc, "id");
    if (!id.length() || id.length() > 32) return fail(r, 400, "socket_id required");
    unregisterDevice(id);
    ok(r, "forget pending", 202);
  });
  postRoute("/provision", true, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    String ssid = field(r, doc, "ssid"), pass = field(r, doc, "pass");
    if (!pass.length()) pass = field(r, doc, "password");
    String lt = field(r, doc, "local_token");
    if (lt.length() && !pc::validLocalToken(lt.c_str())) return fail(r, 400, "invalid local_token");
    if (!ssid.length() && !lt.length()) return fail(r, 400, "ssid or local_token required");
    if (ssid.length() && !settings::setWifi(ssid, pass)) return fail(r, 400, "invalid ssid/password");
    if (lt.length()) {
      if (!settings::setLocalToken(lt)) return fail(r, 500, "token storage failed");
      g_tokenDirty = true;
    }
    if (ssid.length()) { ok(r, "saved; restarting"); g_restart = true; g_restartAt = millis() + 1500; }
    else ok(r, "local token saved");
  });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!requireLocal(r)) return;
    JsonDocument d; telemetry::buildMetrics(d); json(r, 200, d);
  });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!requireLocal(r)) return;
    JsonDocument d;
    d["gateway"] = gateway::state() ? "on" : "off";
    d["cloud_connected"] = backend::connected();
    d["ota_status"] = ota::status();
    d["ota_pending_verify"] = ota::pendingVerify();
    d["offline_pending"] = offlineq::pending();
    auto st = stm32link::stats();
    JsonObject l = d["stm32_link"].to<JsonObject>();
    l["protocol"] = stm32link::protocol() == stm32link::Protocol::Framed ? "framed" : "legacy_text";
    l["online"] = stm32link::online(); l["bytes"] = st.bytes; l["records"] = st.records;
    l["crc_errors"] = st.crcErrors; l["line_overflows"] = st.lineOverflows; l["cmd_timeouts"] = st.cmdTimeouts;
    json(r, 200, d);
  });
  postRoute("/api/inverter/mode", false, [](AsyncWebServerRequest *r, JsonDocument &doc, Cred) {
    if (fid::paymentLocked()) return fail(r, 403, "payment_locked");
    if (stm32link::protocol() != stm32link::Protocol::Framed) return fail(r, 501, "STM32 link is in legacy text mode");
    String m = field(r, doc, "mode");
    int mode = m == "off" ? 0 : m == "grid" ? 1 : m == "inverter" ? 2 : m == "auto" ? 3 : -1;
    if (mode < 0) return fail(r, 400, "mode must be off|grid|inverter|auto");
    g_pendingMode = mode;
    ok(r, "accepted; see /api/state", 202);
  });
  postRoute("/api/inverter/fault_reset", false, [](AsyncWebServerRequest *r, JsonDocument &, Cred) {
    if (stm32link::protocol() != stm32link::Protocol::Framed) return fail(r, 501, "STM32 link is in legacy text mode");
    g_pendingFaultReset = true;
    ok(r, "accepted", 202);
  });
  onboarding::registerRoutes(server);  // firmware 2.1 /v2/* onboarding API
  server.onNotFound([](AsyncWebServerRequest *r) { r->send(404, "text/plain", "not found"); });
  server.begin();
  LOGI("WEB", "BEGIN", "port 80, basic auth user=%s local_token=%s", pc::kLocalUser, token().length() ? "set" : "missing (setup page only)");
}

void tokenChanged() { g_tokenDirty = true; }

void service() {
  refreshToken();
  if (g_pendingMode >= 0) {
    int m = g_pendingMode; g_pendingMode = -1;
    stm32link::setMode(static_cast<pc::frame::Mode>(m));
  }
  if (g_pendingFaultReset) { g_pendingFaultReset = false; stm32link::faultReset(); }
  bool changed = g_stateChanged; g_stateChanged = false;
  sample(changed);
  if (g_restart && (int32_t)(millis() - g_restartAt) >= 0) { ESP.restart(); }
}
}  // namespace web
