#include "net.h"
#include "prefs_safe.h"
#include "ota.h"
#include "backend_ws.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <string>
#include "ca_cert.h"
#include "config.h"
#include "local_auth.h"
#include "ota_gate.h"
#include <heap_guard.h>
#include "heapguard.h"
#include "log.h"
#include "settings.h"
#include "devauth.h"

// Keep a new image in PENDING_VERIFY until the application decides.
extern "C" bool verifyRollbackLater() { return true; }

// Embedded identity string: the installer refuses an image that does not
// carry the expected kind/model/version (prevents cross-flashing).
static const char kIdentity[] __attribute__((used)) =
    "POWERCELL_FW|" POWERCELL_FIRMWARE_KIND "|" POWERCELL_HARDWARE_MODEL "|" POWERCELL_FIRMWARE_VERSION "|END";

namespace ota {
namespace {
constexpr const char *NS = "pc_ota";
bool (*g_prepare)() = nullptr;
bool (*g_acquire)() = nullptr;
std::atomic<bool> g_running{false}, g_updating{false};
bool g_pending = false, g_confirmed = false;
uint32_t g_nextCheck = 0, g_healthySince = 0;
pc::OtaGate g_gate;
String g_status = "idle", g_error, g_deployment, g_installing;
// OTA check transport (WS first, HTTPS fallback) and the TLS memory guards.
pc::OtaTransport g_tx;
pc::TlsBackoff g_checkBackoff, g_dlBackoff;
bool g_forceHttps = false;
// v11 timeouts: TCP connect / TLS handshake 10 s, socket read 15 s.
constexpr uint16_t kConnectMs = 10000, kReadMs = 15000;
// v11: the ota worker runs on core 1 at priority 1. On core 0 (v10) the CPU-bound
// mbedTLS handshake steps (P-384 ECDHE + chain verify, each one uninterrupted call)
// kept priority 1 busy long enough that IDLE0 (priority 0, the only task-WDT-watched
// task) never ran for 5 s -> task_wdt abort. IDLE1 is not WDT-watched and core 1
// round-robins the worker with loop/ws/solar/bms (all priority 1).
constexpr UBaseType_t kOtaPrio = 1;
constexpr BaseType_t kOtaCore = 1;
struct Offer { bool set = false; String version, url, sha256; uint32_t size = 0; };
Offer g_wsOffer;   // written by wsReply (ws task, app lock), consumed by service (main loop, app lock)
Offer g_job;       // the worker's copy; Offer.set=false means "run the HTTPS check first"

void saveStatus(const char *st, const String &err = "") {
  g_status = st; g_error = err;
  Preferences p;
  if (p.begin(NS, false)) { p.putString("job", g_deployment); p.putString("status", g_status); p.putString("error", g_error); p.end(); }
}

// Backend PR #3 (FirmwareService): the hardware key is the shared API_KEY as
// "Authorization: Bearer <API_KEY>"; device secret optional (X-Device-Secret).
#if POWERCELL_OTA_ALLOW_HTTP
// esp32dev_dev only: the dev server has no TLS. Never compiled into other envs.
using OtaClient = WiFiClient;
void prepareClient(OtaClient &) {}
#else
using OtaClient = WiFiClientSecure;
void prepareClient(OtaClient &c) { c.setCACert(POWERCELL_CA_CERT);  // same anchors as the WS (ISRG Root X1 + X2)
  c.setHandshakeTimeout(10); c.setTimeout(15); }
#endif

// Only one credential form is sent: the backend rejects conflicting copies.
// v15: HMAC-signed (devauth.h); x-device-secret too while POWERCELL_LEGACY_SECRET_HEADER.
const char *kCollect[] = {"X-Time-Skew"};
void addHeaders(HTTPClient &http, const String &apiKey, const char *method, const String &url, const String &body) {
  http.addHeader("Authorization", "Bearer " + apiKey);
  http.addHeader("X-Device-ID", devauth::deviceId());
  devauth::Headers h;
  String pq = devauth::pathOf(url);
  if (devauth::sign(method, pq.c_str(), reinterpret_cast<const uint8_t *>(body.c_str()), body.length(), h)) {
    http.addHeader("X-Timestamp", h.ts); http.addHeader("X-Nonce", h.nonce); http.addHeader("X-Signature", h.sig);
  } else {
    static bool warned = false;
    if (!warned) { warned = true; LOGW("OTA", "UNSIGNED", "no secret or no trusted clock: request not signed"); }
  }
#if POWERCELL_LEGACY_SECRET_HEADER
  String secret = settings::cloudSecret();  // value never logged
  if (secret.length()) http.addHeader("x-device-secret", secret);
#endif
  http.collectHeaders(kCollect, 1);
}
// 401 with X-Time-Skew: resync SNTP and retry once.
bool skewRetry(HTTPClient &http, int code, int attempt) {
  if (code != 401 || attempt || !http.hasHeader("X-Time-Skew")) return false;
  LOGW("OTA", "TIME_SKEW", "server skew=%s s: resyncing clock", http.header("X-Time-Skew").c_str());
  http.end();
  devauth::resync();
  devauth::waitSync(5000);
  return true;
}

// POST {base}/api/ota/check  {"device_id":"AABBCCDDEEFF","firmware_version":"2.1.0",...}
bool checkRequest(const String &base, const String &apiKey, String *response) {
  OtaClient client; prepareClient(client);
  HTTPClient http; http.setConnectTimeout(kConnectMs); http.setTimeout(kReadMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  JsonDocument doc;
  doc["device_id"] = settings::id().deviceId;
  doc["firmware_version"] = POWERCELL_FIRMWARE_VERSION;
  doc["kind"] = POWERCELL_FIRMWARE_KIND;      // informational; backend ignores today
  doc["model"] = POWERCELL_HARDWARE_MODEL;
  doc["healthy"] = g_confirmed;
  doc["last_status"] = g_status;
  doc["last_error"] = g_error;
  String body; serializeJson(doc, body);
  int code = -1;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!http.begin(client, base + "/api/ota/check")) return false;
    addHeaders(http, apiKey, "POST", base + "/api/ota/check", body);
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
    if (!skewRetry(http, code, attempt)) break;
  }
#if !POWERCELL_OTA_ALLOW_HTTP
  if (code < 0) {
    char err[96] = {0};
    int tlsErr = client.lastError(err, sizeof err);
    LOGW("OTA", "CHECK_TLS", "tls=%d %s", tlsErr, err);
  }
#endif
  int size = http.getSize();
  bool ok = code == 200 && size != 0 && size <= 4096;  // -1 = chunked, bounded below
  if (ok && response) { *response = http.getString(); ok = response->length() > 0 && response->length() <= 4096; }
  http.end();
  LOGI("OTA", "CHECK", "http=%d free_heap=%lu largest=%lu", code, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  return ok;
}

bool install(const String &base, const String &apiKey, const String &path, const String &digest, const String &newVersion, uint32_t expectSize) {
  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (!part || !pc::isSha256Hex(digest.c_str())) return false;
  OtaClient client; prepareClient(client);
  HTTPClient http; http.setConnectTimeout(kConnectMs); http.setTimeout(kReadMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  int code = -1;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!http.begin(client, base + path)) return false;
    addHeaders(http, apiKey, "GET", base + path, String());
    code = http.GET();
    if (!skewRetry(http, code, attempt)) break;
  }
  int len = http.getSize();  // backend sends Content-Length (Response::file); chunked is refused
  if (code != 200 || len < 288 || static_cast<size_t>(len) > part->size || (expectSize && static_cast<uint32_t>(len) != expectSize)) {
    LOGE("OTA", "DOWNLOAD", "http=%d length=%d partition=%u", code, len, (unsigned)part->size);
    http.end(); return false;
  }
  const size_t size = static_cast<size_t>(len);
  esp_ota_handle_t handle;
  if (esp_ota_begin(part, size, &handle) != ESP_OK) { http.end(); return false; }
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts_ret(&hash, 0);
  static uint8_t buf[2048];
  uint8_t result[32];
  size_t received = 0;
  bool valid = true, identityFound = false;
  std::string tail;
  const std::string expected = std::string("POWERCELL_FW|") + POWERCELL_FIRMWARE_KIND + "|" + POWERCELL_HARDWARE_MODEL + "|" + newVersion.c_str() + "|END";
  uint32_t start = millis(), lastByte = start;
  WiFiClient *stream = http.getStreamPtr();
  while (received < size) {
    if (millis() - start > 180000 || millis() - lastByte > kReadMs) { valid = false; break; }
    int avail = stream->available();
    if (!avail) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    size_t count = std::min(size - received, std::min(sizeof buf, static_cast<size_t>(avail)));
    int rd = stream->read(buf, count);
    if (rd <= 0) { valid = false; break; }
    if (received == 0 && buf[0] != 0xE9) { valid = false; break; }
    mbedtls_sha256_update_ret(&hash, buf, rd);
    tail.append(reinterpret_cast<char *>(buf), rd);
    if (tail.find(expected) != std::string::npos) identityFound = true;
    if (tail.size() > expected.size()) tail.erase(0, tail.size() - expected.size());
    if (esp_ota_write(handle, buf, rd) != ESP_OK) { valid = false; break; }
    received += rd; lastByte = millis();
    vTaskDelay(1);
  }
  mbedtls_sha256_finish_ret(&hash, result); mbedtls_sha256_free(&hash); http.end();
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", result[i]);
  if (!valid || received != size || !identityFound || !digest.equalsIgnoreCase(hex)) {
    esp_ota_abort(handle);
    LOGE("OTA", "VERIFY", "valid=%d received=%u/%u identity=%d sha_ok=%d", valid, (unsigned)received, (unsigned)size, identityFound, digest.equalsIgnoreCase(hex));
    return false;
  }
  if (esp_ota_end(handle) != ESP_OK) return false;
  return esp_ota_set_boot_partition(part) == ESP_OK;
}

// Response (FirmwareService::check): {"status":"success","update_available":bool,
// "device_id","current_version","version","url","sha256","firmware":{version,url,sha256}|null}
void handleOffer(const String &base, const String &key, String newVersion, String url, String digest, uint32_t size);

void check() {
  String base = settings::otaBaseUrl(), key = settings::apiKey();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (WiFi.status() != WL_CONNECTED || key.isEmpty()) return;
  if (g_job.set) { handleOffer(base, key, g_job.version, g_job.url, g_job.sha256, g_job.size); return; }  // offer from ota_available
  String response;
  if (!checkRequest(base, key, &response)) return;
  JsonDocument doc;
  if (deserializeJson(doc, response) || !doc["update_available"].is<bool>()) { LOGW("OTA", "BAD_RESPONSE", "no update_available"); return; }
  if (!doc["update_available"].as<bool>()) return;
  JsonObjectConst fw = doc["firmware"].as<JsonObjectConst>();
  auto pick = [&](const char *k) -> String {
    if (doc[k].is<const char *>()) return doc[k].as<String>();
    if (!fw.isNull() && fw[k].is<const char *>()) return fw[k].as<String>();
    return "";
  };
  String newVersion = pick("version"), url = pick("url"), digest = pick("sha256");
  // A top-level field and the firmware object must agree when both are present.
  if (!fw.isNull()) {
    for (const char *k : {"version", "url", "sha256"}) {
      if (doc[k].is<const char *>() && fw[k].is<const char *>() && doc[k].as<String>() != fw[k].as<String>()) {
        LOGW("OTA", "JOB_REJECTED", "%s mismatch", k); return;
      }
    }
  }
  uint32_t size = doc["size"] | (fw.isNull() ? 0u : (fw["size"] | 0u));
  handleOffer(base, key, newVersion, url, digest, size);
}

// Validate an offer (from the HTTPS check or a WS ota_available) and install it.
void handleOffer(const String &base, const String &key, String newVersion, String url, String digest, uint32_t size) {
  digest.toLowerCase();
  char path[192];
  if (url.isEmpty()) url = "/api/ota/download?device_id=" + String(settings::id().deviceId);
  bool pathOk = pc::otaDownloadPath(base.c_str(), url.c_str(), path, sizeof path);
  if (!pathOk) {
    // Never follow a url to another host. If APP_URL on the server differs from
    // our base, fetch the documented path from our own base instead.
    LOGW("OTA", "URL_FOREIGN", "ignoring url, using %s/api/ota/download", base.c_str());
    String own = "/api/ota/download?device_id=" + String(settings::id().deviceId);
    pathOk = pc::otaDownloadPath(base.c_str(), own.c_str(), path, sizeof path);
  }
  if (!(newVersion.length() && newVersion.length() <= 32 && newVersion != POWERCELL_FIRMWARE_VERSION &&
        pc::isSha256Hex(digest.c_str()) && pathOk)) {
    LOGW("OTA", "JOB_REJECTED", "version=%s sha256=%s url_ok=%d", newVersion.c_str(), pc::isSha256Hex(digest.c_str()) ? "ok" : "missing/invalid", pathOk);
    return;
  }
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (size && next && size > next->size) { LOGW("OTA", "JOB_REJECTED", "size=%lu > partition %lu", (unsigned long)size, (unsigned long)next->size); return; }
  // Ask the main loop to release the WebSocket, then wait (the socket keeps running meanwhile).
  g_installing = newVersion;  // the main loop announces ota_status installing before granting
  g_gate.requestInstall(millis());
  while (!g_gate.mayInstall(millis())) {
    if (!g_gate.waiting()) { LOGW("OTA", "DEFERRED", "backend busy; retrying at the next check"); return; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // The WS is now closed (pauseForOta), so only the download's TLS session will be open.
  // Memory guard: wait (5 s doubling, up to ~75 s total) for a 45 KB block.
  {
    uint32_t t0 = millis();
    while (!heapguard::tlsAllowed(g_dlBackoff, "ota_download")) {
      if (millis() - t0 > 75000) { LOGW("OTA", "DEFERRED", "heap too fragmented for the download; WS reconnects, retry next check"); return; }
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  g_deployment = newVersion;
  g_updating = true;
  Preferences p;
  if (p.begin(NS, false)) { p.putString("expected", newVersion); p.end(); }
  saveStatus("downloading");
  bool prepared = !g_prepare || g_prepare();
  if (prepared && install(base, key, String(path), digest, newVersion, size)) {
    saveStatus("rebooting");
    LOGI("OTA", "REBOOT", "installed %s", newVersion.c_str());
    delay(250);
    ESP.restart();
  }
  saveStatus("failed", prepared ? "download_or_verification_failed" : "safe_output_storage_failed");
  g_updating = false;
}

void worker(void *) {
  check();
  g_gate.finish();
  g_running = false;
  vTaskDelete(nullptr);
}
}  // namespace

void begin(bool (*prepare)(), bool (*acquire)()) {
  g_prepare = prepare; g_acquire = acquire;
  Preferences p;
  String expected;
  if (prefsOpenRO(p, NS)) {
    g_deployment = prefsStr(p, "job"); g_status = prefsStr(p, "status", "idle"); g_error = prefsStr(p, "error");
    expected = prefsStr(p, "expected");
    p.end();
  }
  if (g_status == "downloading") {
    if (expected == POWERCELL_FIRMWARE_VERSION) saveStatus("rebooting");
    else saveStatus("failed", "interrupted_download");
  }
  esp_ota_img_states_t st;
  g_pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  g_confirmed = !g_pending;
  if (g_status == "rebooting" && expected != POWERCELL_FIRMWARE_VERSION) saveStatus("failed", "rolled_back");
  g_nextCheck = millis() + 35000;
  LOGI("OTA", "BEGIN", "%s pending_verify=%d status=%s api_key=%s", kIdentity, g_pending, g_status.c_str(), settings::apiKey().length() ? "set" : "missing");
}

void service(bool healthy) {
  uint32_t now = millis();
  if (g_pending && !g_confirmed) {
    if (!healthy) g_healthySince = 0;
    else if (!g_healthySince) g_healthySince = now;
    if (g_healthySince && now - g_healthySince >= OTA_HEALTHY_MS) {
      if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        g_confirmed = true;
        LOGI("OTA", "CONFIRMED", "image %s marked valid", POWERCELL_FIRMWARE_VERSION);
        if (g_status == "rebooting") saveStatus("idle");
      }
    } else if (now > OTA_VERIFY_DEADLINE_MS) {
      LOGE("OTA", "ROLLBACK", "not healthy within %u ms", (unsigned)OTA_VERIFY_DEADLINE_MS);
      saveStatus("failed", "not_healthy_rolled_back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    return;  // never start another update before this one is confirmed
  }
  if (g_gate.waiting()) {
    // Tell the backend before the WebSocket is released for the download/flash.
    if (g_installing.length()) { backend::sendOtaStatus("installing", g_installing.c_str()); g_installing = ""; }
    g_gate.grant(!g_acquire || g_acquire());
    return;
  }  // install wants the network
  if (g_running) return;
  if (g_wsOffer.set) {  // ota_available over the WS: no HTTPS check needed, go straight to the install path
    g_job = g_wsOffer; g_wsOffer = Offer();
    if (!g_gate.start()) { g_job = Offer(); return; }
    g_running = true;
    if (xTaskCreatePinnedToCore(worker, "ota", 16384, nullptr, kOtaPrio, nullptr, kOtaCore) != pdPASS) { g_running = false; g_gate.finish(); g_job = Offer(); }
    return;
  }
  if (g_tx.waiting()) {
    if (!g_tx.timedOut(now)) return;
    if (!g_tx.httpsAllowed()) {
      // v11: the server supports OTA over WS (or has not been ruled out): retry over the WS
      // once it has been up kSettleMs. No HTTPS fallback just because one reply was lost.
      LOGW("OTA", "WS_CHECK_TIMEOUT", "no ota_available/ota_none within %lu s; retrying over WS (ota_ws_seen=%d)",
           (unsigned long)(pc::OtaTransport::kWsReplyMs / 1000), g_tx.seenYes());
      g_nextCheck = now + 60000;
      return;
    }
    LOGW("OTA", "WS_CHECK_TIMEOUT", "no reply within %lu s and no ota_ws over %u WS cycles; HTTPS check",
         (unsigned long)(pc::OtaTransport::kWsReplyMs / 1000), (unsigned)pc::OtaTransport::kFallbackCycles);
    g_forceHttps = true; g_nextCheck = now;
  }
  if ((int32_t)(now - g_nextCheck) < 0 || WiFi.status() != WL_CONNECTED) return;
  if (net::networkMode() == pc::onb::NetMode::HotspotOnly) return;  // v14: cloud idle, no checks
  if (settings::apiKey().isEmpty()) return;
  // v10 bench (368883): a due 5-min check found the WS just dropped (-76 recv failed);
  // useWs(false) sent it to the HTTPS fallback even though hello_ack had said ota_ws=1.
  // v11: plan() waits for the WS to be back for 30 s instead; HTTPS only per OtaTransport.
  pc::OtaTransport::Plan plan = g_forceHttps ? pc::OtaTransport::Https : g_tx.plan(backend::connected(), backend::connectedForMs());
  if (plan == pc::OtaTransport::Wait) { g_nextCheck = now + 5000; return; }
  if (plan == pc::OtaTransport::Ws) {
    if (backend::sendOtaCheck(g_confirmed, g_status.c_str(), g_error.c_str())) {
      g_tx.sent(now);
      g_nextCheck = now + OTA_CHECK_PERIOD_MS + (esp_random() % 30000);
      LOGI("OTA", "WS_CHECK", "sent ota_check fw=%s", POWERCELL_FIRMWARE_VERSION);
    } else g_nextCheck = now + 15000;
    return;
  }
  // HTTPS fallback (ota_ws never true this boot over 3 WS cycles). One TLS session
  // at a time: wait until the WS handshake is long done and the memory guard passes.
  if (backend::connected() && backend::connectedForMs() < 30000) { g_nextCheck = now + 15000; return; }
  // v10: the WS TLS session (~40 KB) and a second handshake do not fit together: with the
  // WS up, heap_min fell to ~44 KB and mbedTLS failed the ECDSA P-384 chain checks for lack
  // of memory, which it reports as -9984 "X509 certificate verification failed". So the WS
  // is released for the check (it reconnects as soon as the check task ends) and the
  // memory guard is evaluated after that, on the heap the HTTPS session will really get.
  if (backend::connected() && !backend::pauseForOta()) { g_nextCheck = now + 5000; return; }
  if (!heapguard::tlsAllowed(g_checkBackoff, "ota_check")) { g_nextCheck = now + g_checkBackoff.delayMs(); return; }
  LOGI("OTA", "HTTPS_CHECK", "ws released; free_heap=%lu largest=%lu", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  g_forceHttps = false;
  g_nextCheck = now + OTA_CHECK_PERIOD_MS + (esp_random() % 30000);
  g_job = Offer();
  if (!g_gate.start()) return;
  g_running = true;  // the WebSocket keeps running during the check
  if (xTaskCreatePinnedToCore(worker, "ota", 16384, nullptr, kOtaPrio, nullptr, kOtaCore) != pdPASS) { g_running = false; g_gate.finish(); }
}

bool tlsBusy() { return g_running; }
bool busy() { return g_updating; }
bool networkBusy() { return g_gate.wsPaused(); }  // only an install pauses the WebSocket
bool pendingVerify() { return g_pending && !g_confirmed; }
bool confirmed() { return g_confirmed; }
const char *status() { return g_status.c_str(); }
const char *version() { return POWERCELL_FIRMWARE_VERSION; }
void checkSoon() { g_nextCheck = millis() + 1000; }

void wsConnected() { g_tx.wsConnected(); }
void wsDown() {
  if (g_tx.waiting()) { LOGW("OTA", "WS_CHECK_DEFERRED", "WS dropped with ota_check outstanding; retry after reconnect + 30 s"); g_nextCheck = millis(); }
  g_tx.wsDown();
}

void wsSupport(bool otaWs) {
  g_tx.helloAck(otaWs);
  LOGI("OTA", "WS_SUPPORT", "server ota_ws=%d (%s)", otaWs, otaWs ? "checks over WS" : "HTTPS check");
}

// {type:"ota_none"} or {type:"ota_available", version, url, sha256, size}
void wsReply(JsonDocument &doc) {
  if (!g_tx.reply()) { LOGW("OTA", "WS_REPLY_IGNORED", "no ota_check outstanding"); return; }
  String type = doc["type"] | "";
  if (type == "ota_none") { LOGI("OTA", "WS_NONE", "up to date (%s)", POWERCELL_FIRMWARE_VERSION); return; }
  if (g_running || pendingVerify()) { LOGW("OTA", "WS_OFFER_BUSY", "offer ignored: update busy or image not confirmed"); return; }
  Offer o;
  o.version = doc["version"] | ""; o.url = doc["url"] | ""; o.sha256 = doc["sha256"] | "";
  o.size = doc["size"] | 0u;
  if (o.version.isEmpty() || o.sha256.isEmpty()) { LOGW("OTA", "BAD_RESPONSE", "ota_available without version/sha256"); return; }
  if (o.version == POWERCELL_FIRMWARE_VERSION) { LOGI("OTA", "WS_NONE", "offered version is the running one"); return; }
  o.set = true;
  g_wsOffer = o;
  LOGI("OTA", "WS_AVAILABLE", "version=%s size=%lu", o.version.c_str(), (unsigned long)o.size);
}
}  // namespace ota
