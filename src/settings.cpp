#include "prefs_safe.h"
#include "settings.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include "config.h"
#include "identity.h"
#include "local_auth.h"
#include "onb_core.h"
#include <mbedtls/sha256.h>
#include "log.h"
#include "factory_id.h"

namespace settings {
namespace {
Identity g_id;
constexpr const char *NS = "pc_secure";

String get(const char *key, const char *def = "") {
  Preferences p;
  if (!prefsOpenRO(p, NS)) return def;
  String v = prefsStr(p, key, def);
  p.end();
  return v;
}

// Migrate keys written by the 1.0.x firmware (namespace powercell_ota / wifi)
// so existing units keep their backend identity after an OTA to 2.x.
void migrateLegacy() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  if (p.getBool("migrated", false)) { p.end(); return; }
  Preferences old;
  if (prefsOpenRO(old, "powercell_ota")) {
    const char *keys[][2] = {{"api_key", "api_key"}, {"url", "ota_url"}};
    for (auto &k : keys) {
      String v = prefsStr(old, k[0], "");
      if (v.length() && !p.isKey(k[1])) p.putString(k[1], v);
    }
    old.end();
  }
  if (prefsOpenRO(old, "wifi")) {
    String s = prefsStr(old, "ssid", ""), w = prefsStr(old, "pass", "");
    if (s.length() && !p.isKey("wifi_ssid")) { p.putString("wifi_ssid", s); p.putString("wifi_pass", w); }
    old.end();
  }
  p.putBool("migrated", true);
  p.end();
}

String hex(const uint8_t *b, size_t n) {
  String s; char t[3];
  for (size_t i = 0; i < n; ++i) { snprintf(t, sizeof t, "%02x", b[i]); s += t; }
  return s;
}
bool unhex(const String &s, uint8_t *out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; ++i) {
    char buf[3] = {s[2 * i], s[2 * i + 1], 0};
    if (!isxdigit(buf[0]) || !isxdigit(buf[1])) return false;
    out[i] = strtoul(buf, nullptr, 16);
  }
  return true;
}
}  // namespace

void begin() {
  esp_read_mac(g_id.mac, ESP_MAC_WIFI_STA);
  pc::macColon(g_id.mac, g_id.macColon);
  pc::macHex(g_id.mac, g_id.deviceId);
  pc::apSsid(g_id.mac, g_id.apSsid);
  migrateLegacy();
  fid::begin();
  // v15: the factory serial is the display ID and names the hotspot; the MAC-based
  // names remain only for units the station has not registered yet.
  String serial = fid::serial(), name = get("gw_name");
  if (serial.length()) {
    strlcpy(g_id.gatewayName, serial.c_str(), sizeof g_id.gatewayName);
    snprintf(g_id.apSsid, sizeof g_id.apSsid, "Powercell-%s", serial.c_str() + 4);
  } else if (name.length() && name.length() < sizeof g_id.gatewayName) strlcpy(g_id.gatewayName, name.c_str(), sizeof g_id.gatewayName);
  else pc::gatewayName(g_id.mac, g_id.gatewayName);
  LOGI("ID", "DEVICE", "device_id=%s gateway=%s api_key=%s local_token=%s locked=%d", g_id.deviceId, g_id.gatewayName,
       apiKey().length() ? "set" : "MISSING", localToken().length() ? "set" : "missing", provisioningLocked());
}

const Identity &id() { return g_id; }
String getString(const char *key) { return get(key); }
String apiKey() { return get("api_key"); }
// v17: one factory identity secret for WS HMAC and passcode fingerprint (was NVS "dev_secret").
String deviceSecret() { return fid::legacySecretHex(); }
bool validCloudSecret(const String &s) {
  if (s.length() != 64) return false;
  for (size_t i = 0; i < s.length(); ++i) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}
String cloudSecret() { return fid::legacySecretHex(); }  // v15: lives in the factory identity
String otaBaseUrl() {
  String v = get("ota_url", POWERCELL_OTA_BASE_URL_DEFAULT);
  // A cleartext URL left in NVS by an older dev build is ignored by TLS-only builds.
  if (!POWERCELL_OTA_ALLOW_HTTP && !v.startsWith("https://")) v = POWERCELL_OTA_BASE_URL_DEFAULT;
  return v;
}
String wifiSsid() { return get("wifi_ssid"); }
String wifiPass() { return get("wifi_pass"); }

String apPassword() {
  String v = get("ap_pass");
  if (v.length() >= 8) return v;
  v = fid::apPass();  // v15: label password from the factory identity (after a factory reset)
  if (v.length() >= 8) return v;
  // Not provisioned: generate a random per-unit password once and persist it.
  // The factory step reads it with the INFO console command and prints it on
  // the label. It is never derivable from the SSID or MAC.
  // 2.1: 12 random chars without look-alikes (spec section 3).
  uint8_t r[12];
  esp_fill_random(r, sizeof r);
  char out[13];
  pc::onb::apPasswordFromRandom(r, out);
  v = out;
  fid::setApPass(v);  // label value, kept across factory resets
  return v;
}

String localAdminToken() {
  String v = get("admin_tok");
  if (v.length() >= 16) return v;
  // Generate once and persist; printed on the console during factory provisioning.
  uint8_t r[12];
  esp_fill_random(r, sizeof r);
  v = hex(r, sizeof r);
  Preferences p;
  if (p.begin(NS, false)) { p.putString("admin_tok", v); p.end(); }
  return v;
}

String localToken() {
  String v = get("local_tok");
  return pc::validLocalToken(v.c_str()) ? v : String();
}
bool setLocalToken(const String &t) {
  if (!pc::validLocalToken(t.c_str())) return false;
  return setString("local_tok", t);
}
String localTokenFingerprint() {
  String t = localToken();
  if (t.isEmpty()) return "";
  uint8_t d[32];
  mbedtls_sha256_ret(reinterpret_cast<const uint8_t *>(t.c_str()), t.length(), d, 0);
  return hex(d, 8);
}

bool espnowKeys(uint8_t pmk[16], uint8_t lmk[16], uint8_t auth[16]) {
  String v = get("espnow_keys");
  return v.length() == 96 && unhex(v.substring(0, 32), pmk, 16) && unhex(v.substring(32, 64), lmk, 16) && unhex(v.substring(64), auth, 16);
}

bool provisioningLocked() {
  Preferences p;
  if (!prefsOpenRO(p, NS)) return false;
  bool v = p.getBool("locked", false);
  p.end();
  return v;
}

bool setString(const char *key, const String &v) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = v.length() ? p.putString(key, v) == v.length() : (p.isKey(key) ? p.remove(key) : true);
  p.end();
  if (ok && !strcmp(key, "gw_name") && fid::serial().isEmpty() && v.length() && v.length() < sizeof g_id.gatewayName) strlcpy(g_id.gatewayName, v.c_str(), sizeof g_id.gatewayName);
  return ok;
}
bool setWifi(const String &ssid, const String &pass) {
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) return false;
  return setString("wifi_ssid", ssid) && (pass.length() ? setString("wifi_pass", pass) : setString("wifi_pass", ""));
}
bool clearWifi() { return setString("wifi_ssid", "") && setString("wifi_pass", ""); }
bool setEspnowKeys(const String &h) {
  uint8_t tmp[48];
  if (!unhex(h, tmp, 48)) return false;
  String lower = h; lower.toLowerCase();
  return setString("espnow_keys", lower);
}
void wipeUser() {
  // v15/v17 factory reset: everything the customer set. Kept: api_key, ota_url, admin_tok,
  // espnow_keys, locked, migrated (factory/backend provisioning). Identity secret is in pc_ident.
  const char *keys[] = {"wifi_ssid", "wifi_pass", "local_tok", "net_mode", "label_pc", "gw_name", "ap_pass"};
  Preferences p;
  if (!p.begin(NS, false)) return;
  for (auto k : keys) if (p.isKey(k)) p.remove(k);
  p.end();
}
bool lock() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = p.putBool("locked", true);
  p.end();
  return ok;
}
}  // namespace settings
