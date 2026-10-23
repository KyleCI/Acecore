#include "PowercellOTA.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <string>
#include <algorithm>
#include <time.h>

PowercellOTA firmwareOTA;
// Arduino otherwise accepts an OTA image before application setup runs.
extern "C" bool verifyRollbackLater() { return true; }
#ifdef POWERCELL_BOOTSTRAP_CREDENTIALS
static const char bootstrapIdentity[] = "POWERCELL_USB_ONLY_WITH_CREDENTIALS";
#endif
static const char firmwareIdentity[] = "POWERCELL_FW|" POWERCELL_FIRMWARE_KIND
    "|" POWERCELL_HARDWARE_MODEL "|" POWERCELL_FIRMWARE_VERSION "|END";

void PowercellOTA::begin(bool temporaryWifi, const char *caCertificate, bool (*prepareUpdate)()) {
#ifdef POWERCELL_BOOTSTRAP_CREDENTIALS
  Serial.println(bootstrapIdentity);
#endif
  temporary = temporaryWifi;
  ca = caCertificate;
  prepare = prepareUpdate;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[13];
  snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  deviceId = id;
  Preferences prefs;
  prefs.begin("powercell_ota", true);
  baseUrl = prefs.getString("url"); token = prefs.getString("token");
  ssid = prefs.getString("ssid"); password = prefs.getString("password");
  deploymentId = prefs.getString("job"); savedStatus = prefs.getString("status", "idle");
  savedError = prefs.getString("error");
  String expectedVersion = prefs.getString("expected");
  prefs.end();
  if (savedStatus == "downloading") {
    if (expectedVersion == version()) saveStatus("rebooting");
    else saveStatus("failed", "interrupted_download");
  }
  esp_ota_img_states_t state;
  pendingBoot = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK
      && state == ESP_OTA_IMG_PENDING_VERIFY;
  Serial.printf("[OTA] %s id=%s provisioned=%s\n", firmwareIdentity, id, token.length() ? "yes" : "no");
  nextCheck = millis() + 35000;
}

bool PowercellOTA::configure(const String &line) {
  if (!line.startsWith("OTA_CONFIG ")) return false;
  if (running) { Serial.println("[OTA] Busy; retry configuration later"); return true; }
  JsonDocument doc;
  if (deserializeJson(doc, line.substring(11))) { Serial.println("[OTA] Invalid configuration JSON"); return true; }
  String url = doc["base_url"] | "", key = doc["token"] | "";
  String network = doc["ssid"] | "", pass = doc["password"] | "";
  String apiKey = doc["api_key"] | "";
  if (apiKey.length() > 256 || apiKey.indexOf('\r') >= 0 || apiKey.indexOf('\n') >= 0) {
    Serial.println("[OTA] Invalid cloud API key"); return true;
  }
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (!url.startsWith("https://") || url.length() < 12 || url.length() > 180
      || url.indexOf('@') >= 0 || url.indexOf('?') >= 0 || url.indexOf('#') >= 0
      || url.indexOf(' ') >= 0 || key.length() != 64
      || (temporary && (network.isEmpty() || network.length() > 32 || pass.length() > 63))) {
    Serial.println("[OTA] Require HTTPS base_url, 64-character token, and socket Wi-Fi credentials"); return true;
  }
  for (size_t i = 0; i < key.length(); ++i) if (!isxdigit(key[i])) {
    Serial.println("[OTA] Invalid token"); return true;
  }
  Preferences prefs;
  if (!prefs.begin("powercell_ota", false)) { Serial.println("[OTA] Cannot open NVS"); return true; }
  prefs.putString("url", url); prefs.putString("token", key);
  prefs.putString("ssid", network); prefs.putString("password", pass);
  if (apiKey.length() && !temporary) prefs.putString("api_key", apiKey);
  prefs.end();
  baseUrl = url; token = key; ssid = network; password = pass;
  nextCheck = millis() + 1000;
  Serial.println("[OTA] Configuration saved (credentials hidden); reboot after changing the cloud API key");
  return true;
}

void PowercellOTA::service(bool hardwareHealthy) {
  if (pendingBoot && !healthy && millis() > 90000) {
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }
  if (!hardwareHealthy) healthySince = 0;
  else if (!healthySince) healthySince = millis();
  if (!healthy && healthySince && millis() - healthySince >= 30000) {
    if (pendingBoot && esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return;
    healthy = true;
    if (savedStatus == "rebooting") saveStatus("idle");
  }
  if (!healthy || running || token.isEmpty() || baseUrl.isEmpty()
      || static_cast<long>(millis() - nextCheck) < 0) return;
  if (!temporary && WiFi.status() != WL_CONNECTED) return;
  running = true;
  nextCheck = millis() + 300000 + (esp_random() % 30000);
  if (xTaskCreate(worker, "firmware_ota", 16384, this, 1, nullptr) != pdPASS) running = false;
}

void PowercellOTA::worker(void *arg) {
  auto *self = static_cast<PowercellOTA *>(arg);
  self->check();
  self->running = false;
  vTaskDelete(nullptr);
}

void PowercellOTA::saveStatus(const char *status, const String &error) {
  savedStatus = status; savedError = error;
  Preferences prefs;
  if (prefs.begin("powercell_ota", false)) {
    prefs.putString("job", deploymentId); prefs.putString("status", savedStatus);
    prefs.putString("error", savedError); prefs.end();
  }
}

bool PowercellOTA::exchange(const char *status, const String &error, String *response) {
  WiFiClientSecure client; client.setCACert(ca); client.setHandshakeTimeout(12);
  HTTPClient http; http.setConnectTimeout(12000); http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, baseUrl + "/api/firmware/check")) return false;
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["device_id"] = deviceId; doc["kind"] = POWERCELL_FIRMWARE_KIND;
  doc["model"] = POWERCELL_HARDWARE_MODEL; doc["version"] = version();
  doc["healthy"] = healthy; doc["deployment_id"] = deploymentId;
  doc["status"] = status; doc["error"] = error;
  String body; serializeJson(doc, body);
  int code = http.POST(body);
  bool ok = code == 200 && http.getSize() > 0 && http.getSize() <= 4096;
  if (ok && response) *response = http.getString();
  http.end();
  return ok;
}

bool PowercellOTA::install(const String &path, size_t size, const String &digest, const String &newVersion) {
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || size < 288 || size > partition->size || digest.length() != 64) return false;
  WiFiClientSecure client; client.setCACert(ca); client.setHandshakeTimeout(12);
  HTTPClient http; http.setConnectTimeout(12000); http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, baseUrl + path)) return false;
  http.addHeader("Authorization", "Bearer " + token);
  if (http.GET() != 200 || http.getSize() != static_cast<int>(size)) { http.end(); return false; }
  esp_ota_handle_t handle;
  if (esp_ota_begin(partition, size, &handle) != ESP_OK) { http.end(); return false; }
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts_ret(&hash, 0);
  uint8_t buffer[2048], result[32];
  size_t received = 0;
  bool valid = true, identityFound = false;
  std::string tail;
  std::string expected = std::string("POWERCELL_FW|") + POWERCELL_FIRMWARE_KIND + "|"
      + POWERCELL_HARDWARE_MODEL + "|" + newVersion.c_str() + "|END";
  unsigned long start = millis(), lastByte = start;
  auto *stream = http.getStreamPtr();
  while (received < size) {
    if (millis() - start > 180000 || millis() - lastByte > 12000) { valid = false; break; }
    int available = stream->available();
    if (!available) { delay(2); continue; }
    size_t count = std::min(size - received, std::min(sizeof(buffer), static_cast<size_t>(available)));
    int read = stream->read(buffer, count);
    if (read <= 0) { valid = false; break; }
    if (received == 0 && buffer[0] != 0xE9) { valid = false; break; }
    mbedtls_sha256_update_ret(&hash, buffer, read);
    tail.append(reinterpret_cast<char *>(buffer), read);
    if (tail.find(expected) != std::string::npos) identityFound = true;
    if (tail.size() > expected.size()) tail.erase(0, tail.size() - expected.size());
    if (esp_ota_write(handle, buffer, read) != ESP_OK) { valid = false; break; }
    received += read; lastByte = millis(); delay(1);
  }
  mbedtls_sha256_finish_ret(&hash, result); mbedtls_sha256_free(&hash); http.end();
  char hex[65]; for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", result[i]);
  if (!valid || received != size || !identityFound || !digest.equalsIgnoreCase(hex)) {
    esp_ota_abort(handle); return false;
  }
  if (esp_ota_end(handle) != ESP_OK) return false;
  return esp_ota_set_boot_partition(partition) == ESP_OK;
}

void PowercellOTA::check() {
  if (temporary) {
    WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long start = millis();
    while (time(nullptr) < 1700000000 && millis() - start < 15000) delay(50);
    String response;
    if (time(nullptr) >= 1700000000 && exchange(savedStatus.c_str(), savedError, &response)) {
      JsonDocument doc;
      if (!deserializeJson(doc, response) && doc["update"].is<JsonObject>()) {
        JsonObject job = doc["update"];
        String id = job["deployment_id"] | "", path = job["path"] | "";
        String newVersion = job["version"] | "", digest = job["sha256"] | "";
        if (id.length() && id.length() <= 20 && newVersion != version()
            && newVersion.length() <= 32 && path == "/api/firmware/download/" + id
            && job["kind"] == POWERCELL_FIRMWARE_KIND && job["model"] == POWERCELL_HARDWARE_MODEL) {
          deploymentId = id; updating = true;
          Preferences prefs;
          if (prefs.begin("powercell_ota", false)) { prefs.putString("expected", newVersion); prefs.end(); }
          saveStatus("downloading");
          exchange("downloading", "");
          bool prepared = !prepare || prepare();
          if (prepared && install(path, job["size"] | size_t(0), digest, newVersion)) {
            saveStatus("rebooting"); exchange("rebooting", ""); delay(250); ESP.restart();
          }
          saveStatus("failed", prepared ? "download_or_verification_failed" : "safe_output_storage_failed");
          exchange("failed", savedError); updating = false;
        }
      }
    }
  }
  if (temporary) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
}
