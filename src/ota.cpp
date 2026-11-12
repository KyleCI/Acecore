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
#include <memory>
#include <new>
#include <string>
#include <string.h>
#include "ca_cert.h"
#include "config.h"
#include "local_auth.h"
#include "ota_gate.h"
#include "ota_errors.h"
#include <heap_guard.h>
#include "heapguard.h"
#include "log.h"
#include "settings.h"
#include "devauth.h"
#include "lan_ota.h"
#include "power_guard.h"
#include "sockets/sockets.h"
#include "factory_id.h"
#include <ota_boot.h>
#include <Update.h>

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
String g_status = "idle", g_error, g_detail, g_deployment, g_installing;
// OTA check transport (WS first, HTTPS fallback) and the TLS memory guards.
pc::OtaTransport g_tx;
pc::TlsBackoff g_checkBackoff;
bool g_forceHttps = false;
// 2.5.18 timeouts (were 10 s connect / 10 s handshake / 15 s read in v11..2.5.17):
// TCP connect 15 s, TLS handshake 45 s, socket read and body stall 30 s, body total 180 s.
// The api-dev chain (ECDSA P-384, ISRG Root X2) costs the ESP32 several seconds of
// software ECC per handshake; with a congested link the old 10 s cap failed the
// download before any HTTP request was sent (ACE-000001, 12 Nov 2026, 2.5.13).
// The longer cap does not change WDT exposure: the handshake loop in ssl_client.cpp
// yields (vTaskDelay(2)) between mbedTLS steps, the worker stays on core 1 at
// priority 1 (see below), and a longer cap only allows more such steps.
constexpr uint16_t kConnectMs = 15000, kReadMs = 30000;
constexpr uint32_t kHandshakeS = 45, kBodyTotalMs = 180000;
// v11: the ota worker runs on core 1 at priority 1. On core 0 (v10) the CPU-bound
// mbedTLS handshake steps (P-384 ECDHE + chain verify, each one uninterrupted call)
// kept priority 1 busy long enough that IDLE0 (priority 0, the only task-WDT-watched
// task) never ran for 5 s -> task_wdt abort. IDLE1 is not WDT-watched and core 1
// round-robins the worker with loop/ws/solar/bms (all priority 1).
constexpr UBaseType_t kOtaPrio = 1;
constexpr BaseType_t kOtaCore = 1;
// Stack of the "ota" (WS offer / HTTPS check) and "ota_boot" workers: install() runs the
// mbedTLS handshake (P-384 ECDHE + chain verify) on it.
constexpr uint32_t kOtaStack = 16384;
struct Offer { bool set = false; String version, url, sha256; uint32_t size = 0; };
Offer g_wsOffer;   // written by wsReply (ws task, app lock), consumed by service (main loop, app lock)
Offer g_job;       // the worker's copy; Offer.set=false means "run the HTTPS check first"

// detail: short diagnostic for the last failure (http code, mbedTLS error); no secrets.
void saveStatus(const char *st, const String &err = "", const String &detail = "") {
  g_status = st; g_error = err; g_detail = detail;
  Preferences p;
  if (p.begin(NS, false)) {
    p.putString("job", g_deployment); p.putString("status", g_status); p.putString("error", g_error); p.putString("detail", g_detail);
    p.end();
  }
}

// Backend PR #3 (FirmwareService): the hardware key is the shared API_KEY as
// "Authorization: Bearer <API_KEY>"; device secret optional (X-Device-Secret).
#if POWERCELL_OTA_ALLOW_HTTP
// Cleartext HTTP exists only when POWERCELL_OTA_ALLOW_HTTP is set. The product images are TLS.
using OtaClient = WiFiClient;
void prepareClient(OtaClient &) {}
#else
using OtaClient = WiFiClientSecure;
void prepareClient(OtaClient &c) { c.setCACert(POWERCELL_CA_CERT);  // same anchors as the WS (ISRG Root X1 + X2)
  c.setHandshakeTimeout(kHandshakeS); c.setTimeout(kReadMs / 1000); }
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
  if (g_detail.length()) doc["last_error_detail"] = g_detail;
  // 2.5.26: the version the last attempt was for, so the server does not charge another target with it.
  if (g_deployment.length()) doc["last_version"] = g_deployment;
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
  LOGI("OTA", "CHECK", "http=%d free_heap=%lu largest=%lu", code, (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow());
  return ok;
}

// 2.5.22: detail grew to 160 B for the heap numbers; tls = mbedTLS error of a failed connect.
// 2.5.26: len = Content-Length of the response (-1 none), for the stale-job size check.
struct DlResult { pc::otaerr::Fail fail = pc::otaerr::Fail::None; int http = 0, tls = 0; long len = -1; char detail[160] = {0}; };

bool install(const String &base, const String &apiKey, const String &path, const String &digest, const String &newVersion, uint32_t expectSize,
             DlResult &r) {
  using pc::otaerr::Fail;
  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (!part) { r.fail = Fail::NoSlot; LOGE("OTA", "DOWNLOAD", "no OTA partition"); return false; }
  if (!pc::isSha256Hex(digest.c_str())) { r.fail = Fail::VerifySha; snprintf(r.detail, sizeof r.detail, "offer sha256 invalid"); return false; }
  // 2.5.26: the request names the offer (version, sha256); a 2.5.26 server answers 409
  // not_current_offer when it would serve another image. Older servers ignore the parameters.
  char req[320];
  if (!pc::bootota::withOfferQuery(path.c_str(), newVersion.c_str(), digest.c_str(), req, sizeof req)) {
    r.fail = Fail::UrlInvalid; LOGE("OTA", "DOWNLOAD", "download path too long for version/sha256"); return false;
  }
  OtaClient client; prepareClient(client);
  HTTPClient http; http.setConnectTimeout(kConnectMs); http.setTimeout(kReadMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  int code = -1;
  const uint32_t t0 = millis();
  // 2.5.22: DRAM (8-bit) numbers, the heap mbedTLS allocates from; iram = 32-bit-only heap.
  const uint32_t f0 = heapguard::freeNow(), l0 = heapguard::largestNow(), m0 = heapguard::heapMin();
  LOGI("OTA", "DOWNLOAD_TLS_START", "free_heap=%lu largest=%lu min=%lu iram=%lu", (unsigned long)f0, (unsigned long)l0, (unsigned long)m0,
       (unsigned long)heapguard::iramFree());
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!http.begin(client, base + req)) { r.fail = Fail::UrlInvalid; LOGE("OTA", "DOWNLOAD", "url refused by http.begin"); return false; }
    addHeaders(http, apiKey, "GET", base + req, String());
    code = http.GET();
    if (!skewRetry(http, code, attempt)) break;
  }
  r.http = code;
  int len = http.getSize();  // backend sends Content-Length (Response::file); chunked is refused
  r.len = len;
  r.fail = pc::otaerr::classifyResponse(code, len, part->size, expectSize);
  if (r.fail != Fail::None) {
    if (code < 0) {
#if !POWERCELL_OTA_ALLOW_HTTP
      // Same diagnostic as checkRequest. tls=0: failed before TLS (DNS / TCP); tls=-1: handshake timeout.
      char err[96] = {0};
      int tlsErr = client.lastError(err, sizeof err);
      r.tls = tlsErr;
      LOGW("OTA", "DOWNLOAD_TLS", "http=%d tls=%d %s ms=%lu free_heap=%lu largest=%lu", code, tlsErr, err, (unsigned long)(millis() - t0),
           (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow());
      // 2.5.22: the heap at the handshake start goes into last_error_detail.
      snprintf(r.detail, sizeof r.detail, "http=%d tls=%d %.48s", code, tlsErr, err);
      pc::bootota::appendHeap(r.detail, sizeof r.detail, f0, l0, m0);
#else
      snprintf(r.detail, sizeof r.detail, "http=%d", code);
#endif
    } else {
      snprintf(r.detail, sizeof r.detail, "http=%d length=%d expect=%lu slot=%u", code, len, (unsigned long)expectSize, (unsigned)part->size);
    }
    LOGE("OTA", "DOWNLOAD", "http=%d length=%d partition=%u free_heap=%lu largest=%lu", code, len, (unsigned)part->size,
         (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow());
    http.end(); return false;
  }
  const size_t size = static_cast<size_t>(len);
  esp_ota_handle_t handle;
  esp_err_t be = esp_ota_begin(part, size, &handle);
  if (be != ESP_OK) {
    r.fail = Fail::FlashBeginFailed; snprintf(r.detail, sizeof r.detail, "esp_err=0x%x", (unsigned)be);
    LOGE("OTA", "DOWNLOAD", "esp_ota_begin err=0x%x", (unsigned)be);
    http.end(); return false;
  }
  LOGI("OTA", "DOWNLOAD_TLS_UP", "http=%d ms=%lu free_heap=%lu largest=%lu", code, (unsigned long)(millis() - t0), (unsigned long)heapguard::freeNow(),
       (unsigned long)heapguard::largestNow());
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts_ret(&hash, 0);
  // 2.5.20: the 2 KB receive buffer is taken from the heap after the TLS session is up
  // (it was a static array: 2 KB of DRAM held for the whole uptime, which shrinks the heap
  // every TLS handshake needs). If 2 KB is not available, a 512 B stack buffer is used.
  std::unique_ptr<uint8_t[]> heapBuf(new (std::nothrow) uint8_t[2048]);
  uint8_t smallBuf[512];
  uint8_t *buf = heapBuf ? heapBuf.get() : smallBuf;
  const size_t bufLen = heapBuf ? 2048 : sizeof smallBuf;
  if (!heapBuf) LOGW("OTA", "DOWNLOAD_BUF", "2 KB receive buffer unavailable; using 512 B");
  uint8_t result[32];
  size_t received = 0;
  bool identityFound = false;
  Fail loop = Fail::None;
  std::string tail;
  const std::string expected = std::string("POWERCELL_FW|") + POWERCELL_FIRMWARE_KIND + "|" + POWERCELL_HARDWARE_MODEL + "|" + newVersion.c_str() + "|END";
  uint32_t start = millis(), lastByte = start;
  WiFiClient *stream = http.getStreamPtr();
  while (received < size) {
    if (millis() - start > kBodyTotalMs) { loop = Fail::Timeout; break; }
    if (millis() - lastByte > kReadMs) { loop = Fail::Stalled; break; }
    int avail = stream->available();
    if (!avail) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    size_t count = std::min(size - received, std::min(bufLen, static_cast<size_t>(avail)));
    int rd = stream->read(buf, count);
    if (rd <= 0) { loop = Fail::ReadError; break; }
    if (received == 0 && buf[0] != 0xE9) { loop = Fail::VerifyMagic; break; }
    mbedtls_sha256_update_ret(&hash, buf, rd);
    tail.append(reinterpret_cast<char *>(buf), rd);
    if (tail.find(expected) != std::string::npos) identityFound = true;
    if (tail.size() > expected.size()) tail.erase(0, tail.size() - expected.size());
    if (esp_ota_write(handle, buf, rd) != ESP_OK) { loop = Fail::FlashWriteFailed; break; }
    received += rd; lastByte = millis();
    vTaskDelay(1);
  }
  mbedtls_sha256_finish_ret(&hash, result); mbedtls_sha256_free(&hash); http.end();
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", result[i]);
  const bool shaOk = digest.equalsIgnoreCase(hex);
  r.fail = pc::otaerr::classifyVerify(loop, received, size, identityFound, shaOk);
  if (r.fail != Fail::None) {
    esp_ota_abort(handle);
    snprintf(r.detail, sizeof r.detail, "received=%u/%u identity=%d sha_ok=%d ms=%lu", (unsigned)received, (unsigned)size, identityFound, shaOk,
             (unsigned long)(millis() - start));
    LOGE("OTA", "VERIFY", "%s %s", pc::otaerr::name(r.fail), r.detail);
    return false;
  }
  esp_err_t ee = esp_ota_end(handle);
  if (ee != ESP_OK) { r.fail = Fail::FinalizeFailed; snprintf(r.detail, sizeof r.detail, "esp_ota_end=0x%x", (unsigned)ee); return false; }
  ee = esp_ota_set_boot_partition(part);
  if (ee != ESP_OK) { r.fail = Fail::FinalizeFailed; snprintf(r.detail, sizeof r.detail, "set_boot=0x%x", (unsigned)ee); return false; }
  LOGI("OTA", "DOWNLOAD_OK", "bytes=%u ms=%lu", (unsigned)size, (unsigned long)(millis() - t0));
  return true;
}

// ---- 2.5.22 two-phase OTA: the boot job (lib/pc_core/src/ota_boot.h) ----
// NVS pc_ota: bj_ver, bj_path (path on the OTA base), bj_sha, bj_size, bj_try (attempts
// started), bj_run (true = run at the next boot; false = spent, kept for the count),
// ab_sha (abandoned sha256: never staged again until cleared).
// 2.5.26: bj_stg (version of the firmware that staged the job), bj_bld (first 16 hex of its ELF
// sha256). A job without them (staged by 2.5.22..2.5.25) or from another build is stale.
using pc::bootota::Job;
String g_abSha;
bool loadJob(Job &j, bool &run) {
  Preferences p;
  run = false;
  if (!prefsOpenRO(p, NS)) return false;
  String v = prefsStr(p, "bj_ver"), path = prefsStr(p, "bj_path"), sha = prefsStr(p, "bj_sha");
  String stg = prefsStr(p, "bj_stg"), bld = prefsStr(p, "bj_bld");
  j.size = p.getUInt("bj_size", 0); j.tries = p.getUChar("bj_try", 0); run = p.getBool("bj_run", false);
  p.end();
  if (v.isEmpty() && sha.isEmpty()) return false;
  strlcpy(j.version, v.c_str(), sizeof j.version); strlcpy(j.path, path.c_str(), sizeof j.path); strlcpy(j.sha, sha.c_str(), sizeof j.sha);
  strlcpy(j.stager, stg.c_str(), sizeof j.stager); strlcpy(j.build, bld.c_str(), sizeof j.build);
  return true;
}
void saveJob(const Job &j, bool run) {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putString("bj_ver", j.version); p.putString("bj_path", j.path); p.putString("bj_sha", j.sha);
  p.putUInt("bj_size", j.size); p.putUChar("bj_try", j.tries); p.putBool("bj_run", run);
  p.putString("bj_stg", j.stager); p.putString("bj_bld", j.build);
  p.end();
}
void clearJob() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  for (const char *k : {"bj_ver", "bj_path", "bj_sha", "bj_size", "bj_try", "bj_run", "bj_stg", "bj_bld"}) if (p.isKey(k)) p.remove(k);
  p.end();
}
// 2.5.26: first 16 hex of this image's ELF sha256 (esp_app_desc), "" if unavailable.
const char *runningBuild() {
  static char b[17] = {0};
  static bool done = false;
  if (!done) { done = true; if (esp_ota_get_app_elf_sha256(b, sizeof b) <= 0) b[0] = 0; }
  return b;
}
void setAbandoned(const char *sha) {
  g_abSha = sha ? sha : "";
  Preferences p;
  if (!p.begin(NS, false)) return;
  if (g_abSha.length()) p.putString("ab_sha", g_abSha); else if (p.isKey("ab_sha")) p.remove("ab_sha");
  p.end();
}
// Store the job and reboot into the boot path. Returns only when it was not staged (the
// same sha256 already used every attempt): the caller lets the WS reconnect.
void stageAndReboot(const String &version, const char *path, const String &digest, uint32_t size, const char *err, const char *detail) {
  Job prev; bool prevRun = false;
  const bool havePrev = loadJob(prev, prevRun);
  Job j;
  strlcpy(j.version, version.c_str(), sizeof j.version); strlcpy(j.path, path, sizeof j.path); strlcpy(j.sha, digest.c_str(), sizeof j.sha);
  j.size = size;
  strlcpy(j.stager, POWERCELL_FIRMWARE_VERSION, sizeof j.stager); strlcpy(j.build, runningBuild(), sizeof j.build);
  j.tries = pc::bootota::stageTries(havePrev ? prev.sha : "", havePrev ? prev.tries : 0, j.sha);
  if (pc::bootota::stageWouldExceed(j.tries)) {
    LOGE("OTA", "BOOT_ABANDONED", "version=%s: %u boot attempts used; not staged again", j.version, (unsigned)j.tries);
    setAbandoned(j.sha); clearJob();
    char d[160]; snprintf(d, sizeof d, "%s", detail && *detail ? detail : "");
    pc::bootota::appendBoot(d, sizeof d, j.tries, true);
    saveStatus("failed", err && *err ? err : "boot_ota_abandoned", d);
    return;
  }
  saveJob(j, true);
  g_deployment = version;
  saveStatus("boot_pending", err ? err : "", detail ? detail : "");
  LOGW("OTA", "BOOT_STAGED", "version=%s attempt=%u/%u: rebooting to download before the WS, mDNS and web start (free_heap=%lu largest=%lu)",
       j.version, (unsigned)(j.tries + 1), (unsigned)pc::bootota::kMaxTries, (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow());
  delay(250);
  ESP.restart();
}

// 2.5.24: true when staging this sha256 again would exceed kMaxTries (stageAndReboot() would
// abandon it instead of rebooting). Read-only.
bool stageWouldAbandon(const String &digest) {
  Job prev; bool prevRun = false;
  if (!loadJob(prev, prevRun)) return false;
  return pc::bootota::stageWouldExceed(pc::bootota::stageTries(prev.sha, prev.tries, digest.c_str()));
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
  {
    bool haveModel = false;
    String offerModel;
    if (doc["model"].is<const char *>()) { offerModel = doc["model"].as<const char *>(); haveModel = true; }
    if (!fw.isNull() && fw["model"].is<const char *>()) {
      String fm = fw["model"].as<const char *>();
      if (haveModel && fm != offerModel) {
        LOGW("OTA", "MODEL_REJECTED", "check model fields disagree");
        return;
      }
      offerModel = fm;
      haveModel = true;
    }
    if (!haveModel || strcmp(offerModel.c_str(), POWERCELL_HARDWARE_MODEL) != 0) {
      LOGW("OTA", "MODEL_REJECTED", "offer model=%s want=%s", haveModel ? offerModel.c_str() : "(missing)", POWERCELL_HARDWARE_MODEL);
      return;
    }
  }
  handleOffer(base, key, newVersion, url, digest, size);
}

// 2.5.24: the offer rules (ota_boot.h checkOffer), shared by handleOffer() (worker) and the
// main-loop staging path (service(), worker task not created). Lower-cases `digest` and puts
// the checked own-host download path in `path`. Logs JOB_REJECTED for anything but Ok.
pc::bootota::Offer validateOffer(const String &base, const String &newVersion, String url, String &digest, uint32_t size, char *path, size_t cap) {
  using pc::bootota::Offer;
  digest.toLowerCase();
  if (url.isEmpty()) url = "/api/ota/download?device_id=" + String(settings::id().deviceId);
  bool pathOk = pc::otaDownloadPath(base.c_str(), url.c_str(), path, cap);
  if (!pathOk) {
    // Never follow a url to another host. If APP_URL on the server differs from
    // our base, fetch the documented path from our own base instead.
    LOGW("OTA", "URL_FOREIGN", "ignoring url, using %s/api/ota/download", base.c_str());
    String own = "/api/ota/download?device_id=" + String(settings::id().deviceId);
    pathOk = pc::otaDownloadPath(base.c_str(), own.c_str(), path, cap);
  }
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  const Offer v = pc::bootota::checkOffer(newVersion.c_str(), POWERCELL_FIRMWARE_VERSION, digest.c_str(), pathOk, size, next ? next->size : 0,
                                          g_abSha.c_str());
  switch (v) {
    case Offer::Ok: break;
    case Offer::TooBig:
      LOGW("OTA", "JOB_REJECTED", "size=%lu > partition %lu", (unsigned long)size, (unsigned long)next->size);
      break;
    case Offer::Abandoned:
      // 2.5.22: this exact image already used every boot attempt: do not pause the WS for it.
      LOGW("OTA", "JOB_REJECTED", "version=%s abandoned after %u boot attempts (new sha256 or console OTA_RETRY to retry)", newVersion.c_str(),
           (unsigned)pc::bootota::kMaxTries);
      break;
    default:
      LOGW("OTA", "JOB_REJECTED", "%s version=%s sha256=%s url_ok=%d", pc::bootota::offerName(v), newVersion.c_str(),
           pc::isSha256Hex(digest.c_str()) ? "ok" : "missing/invalid", pathOk);
      break;
  }
  return v;
}

// Validate an offer (from the HTTPS check or a WS ota_available) and install it.
void handleOffer(const String &base, const String &key, String newVersion, String url, String digest, uint32_t size) {
  char path[192];
  if (validateOffer(base, newVersion, url, digest, size, path, sizeof path) != pc::bootota::Offer::Ok) return;
  // Ask the main loop to release the WebSocket, then wait (the socket keeps running meanwhile).
  g_installing = newVersion;  // the main loop announces ota_status installing before granting
  g_gate.requestInstall(millis());
  while (!g_gate.mayInstall(millis())) {
    if (!g_gate.waiting()) { LOGW("OTA", "DEFERRED", "backend busy; retrying at the next check"); return; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // The WS is now closed (pauseForOta), so only the download's TLS session will be open.
  // 2.5.22: give the WS task and lwIP a moment to free the old session, log what came back,
  // then the strict gate (8-bit DRAM: free >= 50 KB and both record buffers fit, probed).
  // No room: the job goes to the boot path (ota_boot.h) instead of waiting for a heap
  // that a fragmented DRAM does not give back.
  {
    const uint32_t f1 = heapguard::freeNow(), l1 = heapguard::largestNow();
    vTaskDelay(pdMS_TO_TICKS(1000));
    pc::TlsHeap why = pc::TlsHeap::Ok;
    const bool fits = heapguard::tlsFitsNow(&why);
    LOGI("OTA", "HEAP_WS_RELEASED", "free_heap %lu->%lu largest %lu->%lu min=%lu iram=%lu gate=%s%s", (unsigned long)f1,
         (unsigned long)heapguard::freeNow(), (unsigned long)l1, (unsigned long)heapguard::largestNow(), (unsigned long)heapguard::heapMin(),
         (unsigned long)heapguard::iramFree(), pc::tlsHeapName(why), (why == pc::TlsHeap::Probe && !fits) ? "_failed" : "");
    if (pc::bootota::route(fits) == pc::bootota::Route::Stage) {
      char d[160];
      snprintf(d, sizeof d, "in_place_gate=%s%s", pc::tlsHeapName(why), why == pc::TlsHeap::Probe ? "_failed" : "");
      pc::bootota::appendHeap(d, sizeof d, heapguard::freeNow(), heapguard::largestNow(), heapguard::heapMin());
      stageAndReboot(newVersion, path, digest, size, "", d);
      return;  // abandoned: the WS reconnects
    }
  }
  g_deployment = newVersion;
  g_updating = true;
  Preferences p;
  if (p.begin(NS, false)) { p.putString("expected", newVersion); p.end(); }
  saveStatus("downloading");
  bool prepared = !g_prepare || g_prepare();
  DlResult dl;
  if (prepared && install(base, key, String(path), digest, newVersion, size, dl)) {
    saveStatus("rebooting");
    LOGI("OTA", "REBOOT", "installed %s", newVersion.c_str());
    delay(250);
    ESP.restart();
  }
  if (!prepared) saveStatus("failed", "safe_output_storage_failed");
  else {
    // 2.5.18: specific code (README "OTA error codes"); was download_or_verification_failed.
    char ec[40];
    pc::otaerr::code(dl.fail == pc::otaerr::Fail::None ? pc::otaerr::Fail::FinalizeFailed : dl.fail, dl.http, ec, sizeof ec);
    if (dl.fail == pc::otaerr::Fail::NotCurrent) {
      // 2.5.26: the offer changed between ota_available and the GET: not a failure of either image.
      saveStatus(pc::bootota::kStatusDropped, ec, dl.detail);
      LOGW("OTA", "NOT_CURRENT", "version=%s is no longer the offer (%s); next check retries", newVersion.c_str(), dl.detail);
      g_updating = false;
      return;
    }
    saveStatus("failed", ec, dl.detail);
    LOGE("OTA", "FAILED", "%s %s", ec, dl.detail);
    // 2.5.22: the handshake ran out of heap in place: retry the same job on the boot path.
    if (pc::bootota::inPlaceFallback(dl.fail == pc::otaerr::Fail::ConnectFailed, dl.tls)) {
      g_updating = false;
      stageAndReboot(newVersion, path, digest, size, ec, dl.detail);
    }
  }
  g_updating = false;
}

void worker(void *) {
  check();
  g_gate.finish();
  g_running = false;
  vTaskDelete(nullptr);
}

// 2.5.24: start the "ota" worker; a failure is logged (2.5.22 dropped it silently).
bool startWorker(const char *forWhat) {
  if (xTaskCreatePinnedToCore(worker, "ota", kOtaStack, nullptr, kOtaPrio, nullptr, kOtaCore) == pdPASS) return true;
  LOGW("OTA", "TASK_CREATE_FAILED", "task=ota for=%s stack=%lu free_heap=%lu largest=%lu min=%lu iram=%lu", forWhat, (unsigned long)kOtaStack,
       (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow(), (unsigned long)heapguard::heapMin(),
       (unsigned long)heapguard::iramFree());
  return false;
}

// 2.5.24: a WS offer whose worker task could not be created (ACE-000001, 2.5.22: largest
// block 14836 B < 16384 B stack) goes straight to the boot path from the main loop, with the
// same offer rules (validateOffer) and the same NVS job / attempt count (stageAndReboot).
// The WS is never released here, so a rejected or abandoned offer leaves it untouched.
void stageFromLoop(const Offer &o) {
  String base = settings::otaBaseUrl(), key = settings::apiKey();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  const uint32_t f = heapguard::freeNow(), l = heapguard::largestNow(), m = heapguard::heapMin();
  String digest = o.sha256;
  char path[192];
  if (key.isEmpty()) { LOGW("OTA", "TASK_STAGE_SKIPPED", "version=%s not staged (api_key missing)", o.version.c_str()); return; }
  const pc::bootota::Offer v = validateOffer(base, o.version, o.url, digest, o.size, path, sizeof path);
  if (pc::bootota::onTaskCreateFailed(true, v) != pc::bootota::TaskFail::Stage) {
    LOGW("OTA", "TASK_STAGE_SKIPPED", "version=%s not staged (%s); next check retries", o.version.c_str(), pc::bootota::offerName(v));
    return;
  }
  char d[160];
  pc::bootota::taskStackDetail(d, sizeof d, kOtaStack, f, l, m);
  LOGW("OTA", "TASK_STAGE", "version=%s: ota task stack does not fit; staging to the boot path (%s)", o.version.c_str(), d);
  // Same announcement the worker path makes before the WS is released (not for an image that is abandoned now).
  if (!stageWouldAbandon(digest)) backend::sendOtaStatus("installing", o.version.c_str());
  stageAndReboot(o.version, path, digest, o.size, "", d);
  // Returned: abandoned (status failed/boot_ota_abandoned saved); the WS keeps running.
}

// Boot-time state, once per boot (bootJob() runs before begin()).
bool g_loaded = false;
void loadState() {
  if (g_loaded) return;
  g_loaded = true;
  Preferences p;
  String expected;
  if (prefsOpenRO(p, NS)) {
    g_deployment = prefsStr(p, "job"); g_status = prefsStr(p, "status", "idle"); g_error = prefsStr(p, "error"); g_detail = prefsStr(p, "detail");
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
  if (prefsOpenRO(p, NS)) { g_abSha = prefsStr(p, "ab_sha"); p.end(); }
}

// ---- 2.5.22 boot path ----
struct BootRun { Job job; String base, key; bool (*prepare)() = nullptr; volatile bool done = false; };
BootRun *g_boot = nullptr;

// 2.5.26: drop a stale job: cleared (the attempt is not counted against any image), status
// "dropped" with boot_ota_stale_job (never "failed", so no rollout target is charged with it).
void dropJob(const Job &j, pc::bootota::Stale s, const char *extra) {
  clearJob();
  if (pc::bootota::abandonOnDrop(s)) setAbandoned(j.sha);
  char d[160];
  pc::bootota::dropDetail(d, sizeof d, s, j, extra);
  LOGW("OTA", "BOOT_JOB_DROPPED", "%s running=%s", d, POWERCELL_FIRMWARE_VERSION);
  g_deployment = j.version;
  saveStatus(pc::bootota::kStatusDropped, pc::bootota::kStaleJobError, d);
}

// 2.5.26: boot-time offer check, POST /api/ota/check on the early heap (one TLS session, freed
// before the download starts). Offer: version and lower-case sha256 of the current offer.
pc::bootota::Check bootOfferCheck(const String &base, const String &key, String &version, String &sha) {
  using pc::bootota::Check;
  String response;
  if (!checkRequest(base, key, &response)) return Check::Failed;
  JsonDocument doc;
  if (deserializeJson(doc, response) || !doc["update_available"].is<bool>()) return Check::Failed;
  if (!doc["update_available"].as<bool>()) return Check::None;
  JsonObjectConst fw = doc["firmware"].as<JsonObjectConst>();
  auto pick = [&](const char *k) -> String {
    if (doc[k].is<const char *>()) return doc[k].as<String>();
    if (!fw.isNull() && fw[k].is<const char *>()) return fw[k].as<String>();
    return "";
  };
  version = pick("version"); sha = pick("sha256"); sha.toLowerCase();
  return version.length() ? Check::Offer : Check::Failed;
}

void finishBootFail(BootRun &b, const char *ec, const char *detailIn) {
  const bool abandon = b.job.tries >= pc::bootota::kMaxTries;
  char d[160]; snprintf(d, sizeof d, "%s", detailIn ? detailIn : "");
  pc::bootota::appendBoot(d, sizeof d, b.job.tries, abandon);
  if (abandon) { setAbandoned(b.job.sha); clearJob(); }
  else saveJob(b.job, false);  // spent: kept only for the attempt count of a re-stage
  saveStatus("failed", ec, d);
  LOGE("OTA", "BOOT_FAILED", "%s %s", ec, d);
}

void bootWorker(void *) {
  BootRun &b = *g_boot;
  {
    // 2.5.26 (c): is this job still the server's offer? A stale job is dropped before any download.
    String ov, osha;
    const pc::bootota::Check c = bootOfferCheck(b.base, b.key, ov, osha);
    const pc::bootota::Stale st = pc::bootota::staleAfterCheck(b.job, POWERCELL_FIRMWARE_VERSION, c, ov.c_str(), osha.c_str());
    LOGI("OTA", "BOOT_CHECK", "job=%s check=%s offer=%s verdict=%s free_heap=%lu largest=%lu", b.job.version, pc::bootota::checkName(c),
         ov.length() ? ov.c_str() : "-", pc::bootota::staleName(st), (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow());
    if (st != pc::bootota::Stale::No) {
      char extra[48]; snprintf(extra, sizeof extra, "offer=%.32s", ov.length() ? ov.c_str() : "-");
      dropJob(b.job, st, extra);
      b.done = true;
      vTaskDelete(nullptr);
      return;
    }
  }
  g_deployment = b.job.version;
  g_updating = true;
  { Preferences p; if (p.begin(NS, false)) { p.putString("expected", b.job.version); p.end(); } }
  saveStatus("downloading");
  const bool prepared = !b.prepare || b.prepare();
  DlResult dl;
  if (prepared && install(b.base, b.key, String(b.job.path), String(b.job.sha), String(b.job.version), b.job.size, dl)) {
    clearJob(); setAbandoned("");
    saveStatus("rebooting");
    LOGI("OTA", "REBOOT", "installed %s (boot path, attempt %u)", b.job.version, (unsigned)b.job.tries);
    delay(250);
    ESP.restart();
  }
  if (!prepared) finishBootFail(b, "safe_output_storage_failed", "");
  else {
    // 2.5.26: 409/410 not_current_offer, or a 200 whose length is not the job's size, means the
    // job is stale (the server serves another image): drop it, not a download failure.
    const pc::bootota::Stale st = pc::bootota::staleFromDownload(dl.http, dl.len, b.job.size);
    if (st != pc::bootota::Stale::No) {
      char extra[48]; snprintf(extra, sizeof extra, "http=%d length=%ld expect=%lu", dl.http, dl.len, (unsigned long)b.job.size);
      dropJob(b.job, st, extra);
    } else {
      char ec[40];
      pc::otaerr::code(dl.fail == pc::otaerr::Fail::None ? pc::otaerr::Fail::FinalizeFailed : dl.fail, dl.http, ec, sizeof ec);
      finishBootFail(b, ec, dl.detail);
    }
  }
  g_updating = false;
  b.done = true;
  vTaskDelete(nullptr);
}
}  // namespace

bool bootJob(bool (*prepare)()) {
  using pc::bootota::Boot;
  loadState();
  Job j; bool run = false;
  if (!loadJob(j, run)) return false;
  String base = settings::otaBaseUrl(), key = settings::apiKey();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  char path[192];
  const bool ok = pc::bootota::valid(j, next ? next->size : 0) && next && key.length() &&
                  pc::otaDownloadPath(base.c_str(), j.path, path, sizeof path) && !strcmp(path, j.path);
  const Boot d = pc::bootota::atBoot(run, ok, !strcmp(j.version, POWERCELL_FIRMWARE_VERSION), g_pending, j.tries);
  if (d == Boot::None) return false;
  if (d == Boot::Discard) {
    LOGW("OTA", "BOOT_JOB_DISCARDED", "version=%s valid=%d running=%s pending_verify=%d", j.version, ok, POWERCELL_FIRMWARE_VERSION, g_pending);
    clearJob(); return false;
  }
  // 2.5.26 (a)+(b): a job staged by another firmware (a USB flash or any image change since), or
  // for the running version, is stale. Dropped before Wi-Fi; not counted.
  if (d == Boot::Run) {
    const pc::bootota::Stale st = pc::bootota::staleBeforeCheck(j, POWERCELL_FIRMWARE_VERSION, runningBuild());
    if (st != pc::bootota::Stale::No) { dropJob(j, st, ""); return false; }
  }
  if (d == Boot::Abandon) {
    // Every attempt was started and none reported back (crash, watchdog or power cut).
    LOGE("OTA", "BOOT_ABANDONED", "version=%s: %u attempts started without a result", j.version, (unsigned)j.tries);
    char det[160]; snprintf(det, sizeof det, "last=%s", g_error.length() ? g_error.c_str() : "none");
    pc::bootota::appendBoot(det, sizeof det, j.tries, true);
    setAbandoned(j.sha); clearJob();
    saveStatus("failed", "boot_ota_abandoned", det);
    return false;
  }
  // Run: count the attempt first, so a crash during it is counted too.
  ++j.tries;
  saveJob(j, true);
  LOGI("OTA", "BOOT_JOB", "version=%s attempt=%u/%u free_heap=%lu largest=%lu min=%lu iram=%lu", j.version, (unsigned)j.tries,
       (unsigned)pc::bootota::kMaxTries, (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow(),
       (unsigned long)heapguard::heapMin(), (unsigned long)heapguard::iramFree());
  g_boot = new (std::nothrow) BootRun();
  if (!g_boot) { saveJob(j, false); return false; }
  BootRun &b = *g_boot;
  b.job = j; b.base = base; b.key = key; b.prepare = prepare;
  if (net::networkMode() == pc::onb::NetMode::HotspotOnly) {
    finishBootFail(b, "boot_ota_no_wifi", "hotspot_only");
    delete g_boot; g_boot = nullptr; return true;
  }
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < pc::bootota::kWifiWaitMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    char det[64]; snprintf(det, sizeof det, "no station ip within %lu s", (unsigned long)(pc::bootota::kWifiWaitMs / 1000));
    finishBootFail(b, "boot_ota_no_wifi", det);
    delete g_boot; g_boot = nullptr; return true;
  }
  LOGI("OTA", "BOOT_WIFI", "ip=%s rssi=%d after %lu ms", WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)(millis() - t0));
  // Signed request: same SNTP servers and 30 s budget as the WS (backend::begin starts them again later).
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
  t0 = millis();
  while (fid::hasSecret() && !devauth::clockTrusted() && millis() - t0 < pc::bootota::kClockWaitMs) delay(100);
  if (!devauth::clockTrusted()) LOGW("OTA", "BOOT_CLOCK", "no NTP time after %lu ms; request may be unsigned", (unsigned long)(millis() - t0));
  if (xTaskCreatePinnedToCore(bootWorker, "ota_boot", kOtaStack, nullptr, kOtaPrio, nullptr, kOtaCore) != pdPASS) {
    // 2.5.24: heap figures in the log and in last_error_detail (was an empty detail).
    char det[160];
    snprintf(det, sizeof det, "stack=%lu", (unsigned long)kOtaStack);
    pc::bootota::appendHeap(det, sizeof det, heapguard::freeNow(), heapguard::largestNow(), heapguard::heapMin());
    LOGE("OTA", "TASK_CREATE_FAILED", "task=ota_boot %s", det);
    finishBootFail(b, "boot_ota_task_failed", det);
    delete g_boot; g_boot = nullptr; return true;
  }
  t0 = millis();
  while (!b.done) {
    if (millis() - t0 > pc::bootota::kRunCapMs) { LOGE("OTA", "BOOT_HUNG", "no result after %lu s; rebooting (attempt counted)", (unsigned long)(pc::bootota::kRunCapMs / 1000)); delay(250); ESP.restart(); }
    delay(100);
  }
  delete g_boot; g_boot = nullptr;
  return true;
}

void clearBootJob() { clearJob(); setAbandoned(""); LOGI("OTA", "BOOT_JOB_CLEARED", "boot job and abandoned image cleared (console)"); }

void begin(bool (*prepare)(), bool (*acquire)()) {
  g_prepare = prepare; g_acquire = acquire;
  loadState();
  // A staged job that bootJob() did not run (cleared, invalid or never reached): not pending any more.
  if (g_status == "boot_pending") saveStatus("failed", "boot_ota_not_run", g_detail);
  g_nextCheck = millis() + 35000;
  LOGI("OTA", "BEGIN", "%s pending_verify=%d status=%s api_key=%s", kIdentity, g_pending, g_status.c_str(), settings::apiKey().length() ? "set" : "missing");
}

// ---- Local hotspot OTA (POST /ota), ported from collector 2.5.12 ----
static_assert(OTA_LAN_HEALTHY_MS == pc::lanota::kHotspotHealthyMs, "hotspot confirm time: config.h and lan_ota.h agree");
namespace {
struct Lan {
  bool active = false;
  bool force = false;
  char sha[65] = {0};
  mbedtls_sha256_context h;
  // Exact model in the prefix: an image for another model has no matching tag.
  pc::lanota::TagScanner sc{"POWERCELL_FW|" POWERCELL_FIRMWARE_KIND "|" POWERCELL_HARDWARE_MODEL "|"};
  size_t got = 0;
  uint32_t start = 0, last = 0;
};
Lan *g_lan = nullptr;
}  // namespace

bool lanActive() { return g_lan && g_lan->active; }

void lanAbort(const char *why) {
  if (!g_lan) return;
  if (g_lan->active) {
    Update.abort();
    mbedtls_sha256_free(&g_lan->h);
    LOGE("OTA", "OTA_LAN_FAIL", "%s received=%u", why, (unsigned)g_lan->got);
  }
  delete g_lan; g_lan = nullptr;
  g_updating = false;
  socketsSetPaused(false);
  saveStatus("failed", String("lan_") + why);
}

const char *lanBegin(const char *sha, bool force, int *code) {
  *code = 409;
  if (lanActive()) return "ota_busy";
  if (g_running || g_updating || g_gate.busy()) return "cloud_ota_busy";
  if (g_pending && !g_confirmed) return "current_image_not_confirmed";
  if (powerguard::latched()) return "battery_critical";
  *code = 400;
  if (!sha || !pc::isSha256Hex(sha)) return "x_firmware_sha256_required";
  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  *code = 500;
  if (!part) return "no_ota_partition";
  g_lan = new (std::nothrow) Lan();
  if (!g_lan) { *code = 503; return "out_of_memory"; }
  // Make room: output safe, LoRa task paused, WebSocket released if it is up.
  // networkBusy() stays true for the whole upload, so the backend does not reconnect.
  g_lan->active = true;
  if (g_prepare) g_prepare();
  socketsSetPaused(true);
  if (backend::connected() && g_acquire) g_acquire();
  if (!Update.begin(part->size, U_FLASH)) {
    // 2.5.24: Update.begin() mallocs its 4 KB sector buffer and fails with err=0 (UPDATE_ERROR_OK)
    // when that does not fit: reported as 503 update_begin_out_of_memory (was 500 update_begin_failed).
    const uint8_t ue = Update.getError();
    LOGE("OTA", "OTA_LAN_FAIL", "update_begin err=%u free_heap=%lu largest=%lu", ue, (unsigned long)heapguard::freeNow(),
         (unsigned long)heapguard::largestNow());
    delete g_lan; g_lan = nullptr; socketsSetPaused(false);
    if (ue == UPDATE_ERROR_OK) { *code = 503; return "update_begin_out_of_memory"; }
    *code = 500; return "update_begin_failed";
  }
  strlcpy(g_lan->sha, sha, sizeof g_lan->sha);
  g_lan->force = force;
  mbedtls_sha256_init(&g_lan->h); mbedtls_sha256_starts_ret(&g_lan->h, 0);
  g_lan->start = g_lan->last = millis();
  g_updating = true;
  saveStatus("lan_uploading");
  LOGI("OTA", "OTA_LAN_BEGIN", "running=%s model=%s partition=%s size=%u force=%d free_heap=%lu", POWERCELL_FIRMWARE_VERSION,
       POWERCELL_HARDWARE_MODEL, part->label, (unsigned)part->size, force, (unsigned long)heapguard::freeNow());
  *code = 0;
  return nullptr;
}

const char *lanWrite(const uint8_t *d, size_t n, int *code) {
  if (!lanActive()) { *code = 409; return "no_upload"; }
  if (g_lan->got == 0 && n && d[0] != 0xE9) { *code = 400; lanAbort("not_esp32_image"); return "not_esp32_image"; }
  mbedtls_sha256_update_ret(&g_lan->h, d, n);
  g_lan->sc.feed(d, n);
  if (Update.write(const_cast<uint8_t *>(d), n) != n) {
    *code = Update.getError() == UPDATE_ERROR_SPACE ? 413 : 500;
    lanAbort("flash_write"); return "flash_write_failed";
  }
  g_lan->got += n; g_lan->last = millis();
  *code = 0;
  return nullptr;
}

const char *lanEnd(int *code, String *version) {
  if (!lanActive()) { *code = 409; return "no_upload"; }
  uint8_t res[32]; char hex[65];
  mbedtls_sha256_finish_ret(&g_lan->h, res);
  for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", res[i]);
  if (g_lan->got < 288) { *code = 400; lanAbort("too_small"); return "image_too_small"; }
  if (!pc::lanota::hexEq(hex, g_lan->sha)) { *code = 422; lanAbort("sha256_mismatch"); return "sha256_mismatch"; }
  pc::lanota::Verdict v = pc::lanota::judge(POWERCELL_FIRMWARE_VERSION, g_lan->sc, g_lan->force);
  if (v != pc::lanota::Verdict::Ok) {
    const char *e = pc::lanota::verdictName(v);
    *code = v == pc::lanota::Verdict::NoTag ? 422 : 409;
    if (version) *version = g_lan->sc.version().c_str();
    lanAbort(e); return e;
  }
  String ver = g_lan->sc.version().c_str();
  if (version) *version = ver;
  // sha + model identity + version verified: only now validate the image and set the boot partition.
  if (!Update.end(true)) {
    LOGE("OTA", "OTA_LAN_FAIL", "update_end err=%u", Update.getError());
    mbedtls_sha256_free(&g_lan->h); delete g_lan; g_lan = nullptr; g_updating = false; socketsSetPaused(false);
    saveStatus("failed", "lan_update_end"); *code = 500; return "image_invalid";
  }
  mbedtls_sha256_free(&g_lan->h);
  LOGI("OTA", "OTA_LAN_OK", "version=%s bytes=%u ms=%lu sha256=%s; reboot after reply", ver.c_str(), (unsigned)g_lan->got,
       (unsigned long)(millis() - g_lan->start), hex);
  delete g_lan; g_lan = nullptr;
  // Same rollback bookkeeping as the cloud path: begin() turns a rolled-back boot into failed/rolled_back.
  g_deployment = ver;
  Preferences p;
  if (p.begin(NS, false)) { p.putString("expected", ver); p.end(); }
  saveStatus("rebooting");
  *code = 200;
  return nullptr;  // g_updating stays true: nothing else starts before the reboot
}

void service(bool healthy) {
  uint32_t now = millis();
  if (lanActive()) {
    if (now - g_lan->last > OTA_LAN_IDLE_MS) lanAbort("upload_stalled");
    return;
  }
  // hotspot_only has no cloud, so backend::healthy() is never true there: a healthy local
  // run of OTA_LAN_HEALTHY_MS confirms the image. Other modes keep the 30 s cloud rule.
  const bool hotspotOnly = net::networkMode() == pc::onb::NetMode::HotspotOnly;
  const uint32_t needMs = pc::lanota::confirmAfterMs(hotspotOnly, OTA_HEALTHY_MS);
  healthy = pc::lanota::healthyNow(hotspotOnly, healthy, ESP.getFreeHeap());
  if (g_pending && !g_confirmed) {
    if (!healthy) g_healthySince = 0;
    else if (!g_healthySince) g_healthySince = now;
    if (g_healthySince && now - g_healthySince >= needMs) {
      if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        g_confirmed = true;
        LOGI("OTA", "CONFIRMED", "image %s marked valid (%s)", POWERCELL_FIRMWARE_VERSION, hotspotOnly ? "hotspot_only local run" : "cloud healthy");
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
    if (!startWorker("ws_offer")) {
      g_running = false; g_gate.finish();
      const Offer o = g_job; g_job = Offer();
      stageFromLoop(o);  // reboots when staged; returns (WS untouched) when rejected
    }
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
    if (backend::sendOtaCheck(g_confirmed, g_status.c_str(), g_error.c_str(), g_detail.c_str(), g_deployment.c_str())) {
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
  LOGI("OTA", "HTTPS_CHECK", "ws released; free_heap=%lu largest=%lu", (unsigned long)heapguard::freeNow(), (unsigned long)heapguard::largestNow());
  g_forceHttps = false;
  g_nextCheck = now + OTA_CHECK_PERIOD_MS + (esp_random() % 30000);
  g_job = Offer();
  if (!g_gate.start()) return;
  g_running = true;  // the WebSocket keeps running during the check
  if (!startWorker("https_check")) { g_running = false; g_gate.finish(); }  // logged; the next check period retries
}

bool tlsBusy() { return g_running; }
bool busy() { return g_updating; }
bool networkBusy() { return g_gate.wsPaused() || lanActive(); }  // only an install (cloud or POST /ota) pauses the WebSocket
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
  if (!doc["model"].is<const char *>() || strcmp(doc["model"].as<const char *>(), POWERCELL_HARDWARE_MODEL) != 0) {
    LOGW("OTA", "MODEL_REJECTED", "offer model=%s want=%s",
         doc["model"].is<const char *>() ? doc["model"].as<const char *>() : "(missing)", POWERCELL_HARDWARE_MODEL);
    return;
  }
  o.set = true;
  g_wsOffer = o;
  LOGI("OTA", "WS_AVAILABLE", "version=%s size=%lu", o.version.c_str(), (unsigned long)o.size);
}
}  // namespace ota
