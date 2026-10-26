#include <esp_heap_caps.h>
#include "net.h"
#include <WiFi.h>
#include "config.h"
#include "log.h"
#include "onb_core.h"
#include "settings.h"

namespace net {
namespace {
enum class Test : uint8_t { Idle, Pending, Running, Ok, Failed };
bool g_ap = false, g_wasUp = false;
uint32_t g_lastRetry = 0, g_apRestartAt = 0;
String g_ssid;
pc::onb::ApPolicy g_policy;
volatile bool g_forceReq = false, g_scanReq = false;

SemaphoreHandle_t g_mx = nullptr;
struct Lock { Lock() { xSemaphoreTakeRecursive(g_mx, portMAX_DELAY); } ~Lock() { xSemaphoreGiveRecursive(g_mx); } };
volatile Test g_test = Test::Idle;
String g_tSsid, g_tPass, g_tId, g_tReason, g_tIp;
bool g_t5g = false;
volatile bool g_tAssoc = false;
volatile uint8_t g_tDisc = 0;
uint32_t g_tStart = 0;
int g_tRssi = 0, g_tCh = 0;
String g_scanJson = "[]";
uint32_t g_scanAt = 0;
pc::onb::NetMode g_mode = pc::onb::NetMode::Unset;  // v14
bool hotspotOnlyNow() { return g_mode == pc::onb::NetMode::HotspotOnly; }

void startAp() {
  if (g_ap) return;
  WiFi.mode(WIFI_AP_STA);
  String pass = settings::apPassword();
  WiFi.softAP(settings::id().apSsid, pass.c_str());
  g_ap = true;
  LOGW("WIFI", "SETUP_AP", "ssid=%s ip=%s state=%s (password on the unit label)", settings::id().apSsid, WiFi.softAPIP().toString().c_str(), stateName());
}
void stopAp() {
  if (!g_ap) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_ap = false;
  // v13: free setup-only scan state (the scan list String grows during onboarding and
  // stayed allocated for the whole uptime, splitting the heap)
  { Lock l; WiFi.scanDelete(); g_scanJson = String(); g_scanAt = 0; }
  LOGI("WIFI", "SETUP_AP_OFF", "station connected free_heap=%lu largest=%lu", (unsigned long)ESP.getFreeHeap(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
void onWifiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) g_tDisc = info.wifi_sta_disconnected.reason;
  else if (e == ARDUINO_EVENT_WIFI_STA_CONNECTED) g_tAssoc = true;
}
void restoreSaved() {
  if (hotspotOnlyNow()) { WiFi.disconnect(false, false); return; }  // v14: no station attempts
  if (g_ssid.length()) WiFi.begin(g_ssid.c_str(), settings::wifiPass().c_str());
  else WiFi.disconnect(false, false);
}
void serviceTest(uint32_t now) {
  if (g_test == Test::Pending) {
    String s, p;
    { Lock l; s = g_tSsid; p = g_tPass; }
    startAp();
    WiFi.disconnect(false, false);
    g_tAssoc = false; g_tDisc = 0; g_tStart = now;
    WiFi.begin(s.c_str(), p.c_str());
    g_test = Test::Running;
    LOGI("WIFI", "TEST_START", "id=%s ssid=%s", g_tId.c_str(), s.c_str());
    return;
  }
  if (g_test != Test::Running) return;
  const char *reason = nullptr;
  bool ok = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  if (!ok) {
    uint8_t d = g_tDisc;
    if (d) reason = pc::onb::wifiFailReason(d, g_t5g);
    if (!reason && now - g_tStart >= WIFI_TEST_TIMEOUT_MS) reason = g_tAssoc ? "no_ip" : d ? "timeout" : "timeout";
    if (!reason) return;
  }
  Lock l;
  if (ok) {
    g_tIp = WiFi.localIP().toString(); g_tRssi = WiFi.RSSI(); g_tCh = WiFi.channel(); g_tReason = "";
    g_test = Test::Ok;
    LOGI("WIFI", "TEST_OK", "id=%s ip=%s rssi=%d ch=%d", g_tId.c_str(), g_tIp.c_str(), g_tRssi, g_tCh);
  } else {
    g_tReason = reason; g_test = Test::Failed;
    LOGW("WIFI", "TEST_FAILED", "id=%s reason=%s disc=%u", g_tId.c_str(), reason, (unsigned)g_tDisc);
    g_tPass = "";
    restoreSaved();
  }
}
}  // namespace

void begin() {
  g_mx = xSemaphoreCreateRecursiveMutex();
  WiFi.persistent(false);  // credentials live in our NVS namespace, not the SDK's
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(settings::id().gatewayName);
  WiFi.onEvent(onWifiEvent);
  g_policy.fallbackMs = uint32_t(AP_FALLBACK_MIN) * 60000UL;
  g_policy.begin(millis());
  g_ssid = settings::wifiSsid();
  g_mode = pc::onb::netModeFromStored(settings::getString("net_mode").c_str());
  if (hotspotOnlyNow()) {
    WiFi.setAutoReconnect(false);
    LOGI("WIFI", "HOTSPOT_ONLY", "network_mode=hotspot_only: hotspot stays on, no station, cloud idle");
    startAp(); return;
  }
  if (g_ssid.isEmpty()) { LOGW("WIFI", "UNPROVISIONED", "no saved network"); startAp(); return; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_ssid.c_str(), settings::wifiPass().c_str());
  LOGI("WIFI", "CONNECTING", "ssid=%s", g_ssid.c_str());
}

void service() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !g_wasUp) LOGI("WIFI", "UP", "ip=%s rssi=%d ch=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  if (!up && g_wasUp) LOGW("WIFI", "DOWN", "reconnecting");
  g_wasUp = up;
  if (g_forceReq) { g_forceReq = false; g_policy.force(now); LOGW("WIFI", "AP_ON", "hotspot forced on for %lu s", (unsigned long)(g_policy.forceMs / 1000)); }
  serviceTest(now);
  bool testing = g_test == Test::Pending || g_test == Test::Running;
  if (g_apRestartAt && (int32_t)(now - g_apRestartAt) >= 0) {
    g_apRestartAt = 0;
    // v12: /v2/ap_password during the post-save grace restarted the AP at about +50 s and
    // logged it as SETUP_AP state=fallback while the STA and WS were up. With the station
    // connected (not testing/forced), keep the running AP; the new password applies the
    // next time the hotspot starts (it turns off at the end of the grace anyway).
    if (g_ap && up && !testing && !g_policy.forced) LOGI("WIFI", "AP_RESTART_SKIPPED", "station connected; new hotspot password applies at the next start");
    else if (g_ap) { WiFi.softAPdisconnect(false); g_ap = false; startAp(); }
  }
  bool want = g_policy.update(g_ssid.length() > 0, up, testing, now, hotspotOnlyNow());
  if (want && !g_ap) startAp();
  if (!want && g_ap) stopAp();
  if (g_scanReq && !testing) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true, false, false, 300);
    else if (n >= 0) {
      JsonDocument d; JsonArray a = d.to<JsonArray>();
      for (int i = 0; i < n && i < 20; ++i) {
        if (WiFi.SSID(i).isEmpty()) continue;
        JsonObject o = a.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i); o["rssi"] = WiFi.RSSI(i); o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      }
      WiFi.scanDelete();
      String s; serializeJson(d, s);
      Lock l; g_scanJson = s; g_scanAt = now; g_scanReq = false;
    }
  }
  if (g_ssid.isEmpty() || testing || g_test == Test::Ok) return;
  if (!up && now - g_lastRetry >= WIFI_CONNECT_TIMEOUT_MS) { g_lastRetry = now; WiFi.reconnect(); }
}

bool staConnected() { return WiFi.status() == WL_CONNECTED; }
// Backend WS may run only on a saved network with no Wi-Fi test in progress or
// awaiting save: the TLS handshake (~40 KB heap) plus AP_STA plus the phone's
// polling starved the heap and crashed the async web server (fw 2.2.0 bench).
bool backendAllowed() {
  Test t = g_test;
  return g_ssid.length() > 0 && t != Test::Pending && t != Test::Running && t != Test::Ok;
}
bool apActive() { return g_ap; }
const char *stateName() {
  bool testing = g_test == Test::Pending || g_test == Test::Running;
  return pc::onb::ApPolicy::stateName(g_ssid.length() > 0, WiFi.status() == WL_CONNECTED, testing, g_ap, hotspotOnlyNow());
}
pc::onb::NetMode networkMode() { return g_mode; }
bool setNetworkMode(pc::onb::NetMode m) {
  using pc::onb::NetMode;
  if (m == NetMode::Unset) return false;
  Lock l;
  if (m == g_mode) return true;
  if (!settings::setString("net_mode", pc::onb::netModeName(m))) return false;
  NetMode was = g_mode; g_mode = m;
  if (m == NetMode::HotspotOnly) {
    WiFi.setAutoReconnect(false);
    if (g_test != Test::Pending && g_test != Test::Running) WiFi.disconnect(false, false);
    LOGI("WIFI", "NETWORK_MODE", "hotspot_only (was %s): hotspot stays on, station off, cloud idle", pc::onb::netModeName(was));
  } else {
    WiFi.setAutoReconnect(true);
    g_policy.restartGrace(millis());  // existing grace: AP off 120 s after the station is up
    if (was == NetMode::HotspotOnly && g_ssid.length() && g_test != Test::Pending && g_test != Test::Running) WiFi.begin(g_ssid.c_str(), settings::wifiPass().c_str());
    LOGI("WIFI", "NETWORK_MODE", "station (was %s)%s", pc::onb::netModeName(was), g_ssid.length() ? "" : ": no saved network yet, hotspot stays on");
  }
  return true;
}
void forceAp() { g_forceReq = true; }
void restartAp(uint32_t ms) { g_apRestartAt = millis() + ms; if (!g_apRestartAt) g_apRestartAt = 1; }

bool startTest(const String &ssid, const String &pass, bool fiveG, String &id) {
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63 || (pass.length() && pass.length() < 8)) return false;
  Lock l;
  if (g_test == Test::Pending || g_test == Test::Running) return false;
  uint8_t r[4]; esp_fill_random(r, sizeof r);
  char h[9]; snprintf(h, sizeof h, "%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
  g_tId = h; g_tSsid = ssid; g_tPass = pass; g_t5g = fiveG; g_tReason = ""; g_tIp = "";
  g_test = Test::Pending;
  id = g_tId;
  return true;
}
void testStatus(JsonDocument &d) {
  Lock l;
  Test t = g_test;
  d["state"] = t == Test::Idle ? "idle" : t == Test::Ok ? "ok" : t == Test::Failed ? "failed" : "testing";
  if (g_tId.length()) d["test_id"] = g_tId;
  if (t == Test::Failed) d["reason"] = g_tReason;
  if (t == Test::Ok) { d["ip"] = g_tIp; d["rssi"] = g_tRssi; d["channel"] = g_tCh; }
}
bool saveTested(String &ip) {
  Lock l;
  if (g_test != Test::Ok || !settings::setWifi(g_tSsid, g_tPass)) return false;
  g_ssid = g_tSsid; ip = g_tIp;
  g_tPass = ""; g_test = Test::Idle;
  g_policy.restartGrace(millis());
  if (g_mode != pc::onb::NetMode::Station) setNetworkMode(pc::onb::NetMode::Station);  // v14: save implies station
  LOGI("WIFI", "SAVED", "ssid=%s ip=%s hotspot off in 120 s", g_ssid.c_str(), ip.c_str());
  return true;
}
bool scan(JsonArray out) {
  Lock l;
  bool fresh = g_scanAt && millis() - g_scanAt < 30000;
  if (!fresh) g_scanReq = true;
  JsonDocument d; deserializeJson(d, g_scanJson);
  for (JsonVariant v : d.as<JsonArray>()) out.add(v);
  return fresh;
}
}  // namespace net
