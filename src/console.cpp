#include "power_guard.h"
#include "console.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "log.h"
#include "net.h"
#include "onboarding.h"
#include "ota.h"
#include "settings.h"
#include "sockets/sockets.h"
#include "stm32_link.h"
#include "web_local.h"
#include <WiFi.h>
#include "backend_ws.h"
#include "bms.h"
#include "devauth.h"
#include "factory_id.h"

namespace console {
namespace {
char line[600];
size_t len = 0;
bool overflow = false;
uint32_t resetArmedAt = 0;

bool isSecretKey(const String &k) {
  return k == "api_key" || k == "dev_secret" || k == "device_secret" || k == "ap_pass" || k == "admin_tok" || k == "espnow_keys" || k == "ota_url";
}

void info() {
  const auto &id = settings::id();
  bool locked = settings::provisioningLocked();
  Serial.printf("device_id=%s mac=%s gateway=%s fw=%s\n", id.deviceId, id.macColon, id.gatewayName, ota::version());
  // v15 factory identity (the secret value is never printed here; see SECRET)
  String ser = fid::serial();
  Serial.printf("serial=%s ssid=%s secret=%s secret_reported=%d reset_pending=%d clock_trusted=%d\n", ser.length() ? ser.c_str() : "unset",
                id.apSsid, fid::hasSecret() ? "set" : "MISSING", fid::secretReported(), fid::resetInfo().pending, devauth::clockTrusted());
  // v12: tokens are never printed, only set/missing
  // v17: one factory identity secret (see secret= on the line above); no separate dev_secret=
  Serial.printf("local_token=%s\n", settings::localTokenFingerprint().length() ? "set" : "missing");
  Serial.printf("api_key=%s wifi_ssid=%s locked=%d\n",
                settings::apiKey().length() ? "set" : "MISSING",
                settings::wifiSsid().c_str(), locked);
  Serial.printf("ap_ssid=%s wifi_state=%s hotspot=%d\n", id.apSsid, net::stateName(), net::apActive());
  String ofp = onboarding::fingerprint();
  Serial.printf("onboarding=2 passcode=%s default=%d claimed=%d fingerprint=%s\n", onboarding::passcodeSet() ? "set" : "unset",
                onboarding::passcodeDefault(), onboarding::claimed(), ofp.length() ? ofp.c_str() : "null");
  if (!locked) {
    // Factory label step only. After LOCK these are never printed again.
    String adm = settings::localAdminToken();
    String admMask = adm.length() >= 8 ? "set(..." + adm.substring(adm.length() - 4) + ")" : adm.length() ? "set" : "missing";
    Serial.printf("LABEL ap_pass=%s admin_token=%s\n", settings::apPassword().c_str(), admMask.c_str());
    Serial.printf("LABEL %s\n", onboarding::labelLine().c_str());
    Serial.printf("LABEL_QR WIFI:T:WPA;S:%s;P:%s;;\n", id.apSsid, settings::apPassword().c_str());
  }
}

void set(const String &key, const String &value) {
  if (isSecretKey(key) && settings::provisioningLocked()) { Serial.println("ERR locked; FACTORY_WIPE first"); return; }
  bool ok = false;
  if (key == "passcode") { Serial.println("ERR v15: no factory passcode; the customer sets it in app setup"); return; }
  if (key == "stm32_proto") {
    Preferences p;
    if ((value == "0" || value == "1") && p.begin("pc_cfg", false)) { ok = p.putUChar("stm32_proto", value.toInt()); p.end(); }
  } else if (key == "espnow_keys") ok = settings::setEspnowKeys(value);
  else if (key == "local_tok") {
    // Allowed after LOCK too: USB access is physical presence, and setting it reveals nothing.
    ok = settings::setLocalToken(value);
    if (ok) { web::tokenChanged(); Serial.println("OK local_token set"); return; }
  } else if (key == "device_secret") {
    String v = value; v.trim(); v.toLowerCase();
    // v15 rework/rotate: the station already holds this value, so it counts as reported.
    ok = settings::validCloudSecret(v) && fid::setSecretHex(v);
    Serial.println(ok ? "OK device_secret set (reboot to apply)" : "ERR device_secret must be 64 hex");
    return;
  } else if (key == "dev_secret") {
    // v17: merged into factory identity (SECRET / SET device_secret). Refuse so operators do not
    // think a second HMAC key still exists. Value is never stored or logged.
    Serial.println("ERR v17: use SECRET or SET device_secret (factory identity); dev_secret merged");
    return;
  }
  else if (key == "ota_url") ok = (value.startsWith("https://") || (POWERCELL_OTA_ALLOW_HTTP && value.startsWith("http://"))) && value.indexOf('@') < 0 && settings::setString("ota_url", value);
  else if (key == "ap_pass") ok = value.length() >= 8 && value.length() <= 63 && fid::setApPass(value) && settings::setString("ap_pass", "");
  else if (key == "admin_tok") ok = value.length() >= 16 && settings::setString("admin_tok", value);
  else if (key == "api_key") ok = value.length() <= 256 && value.indexOf('\r') < 0 && settings::setString("api_key", value);
  else if (key == "gw_name") ok = value.length() > 0 && value.length() < 24 && settings::setString("gw_name", value);
  else if (key == "wifi_ssid") ok = value.length() <= 32 && settings::setString("wifi_ssid", value);
  else if (key == "wifi_pass") ok = value.length() <= 63 && settings::setString("wifi_pass", value);
  Serial.println(ok ? "OK (reboot to apply)" : "ERR invalid key or value");
}

void otaConfig(const String &json) {  // 1.0.x factory format
  if (settings::provisioningLocked()) { Serial.println("ERR locked"); return; }
  JsonDocument d;
  if (deserializeJson(d, json)) { Serial.println("ERR json"); return; }
  if (d["base_url"].is<const char *>()) set("ota_url", d["base_url"].as<String>());
  // "token" (1.0.x 64-hex OTA bearer) is obsolete: the backend OTA now takes API_KEY as Bearer.
  if (d["local_token"].is<const char *>()) set("local_tok", d["local_token"].as<String>());
  if (d["api_key"].is<const char *>()) set("api_key", d["api_key"].as<String>());
  if (d["ssid"].is<const char *>()) { set("wifi_ssid", d["ssid"].as<String>()); set("wifi_pass", d["password"] | ""); }
}

void stm32(const String &arg) {
  if (arg == "STATUS") {
    pc::Stm32Status s; uint32_t age = 0;
    if (!stm32link::status(s, &age)) { Serial.println("no status yet"); return; }
    Serial.printf("sys=%s sw=%s chr=%s bat=%.2fV/%u%% grid=%.1fV temp=%.1fC vout=%.1f iout=%.2f fault=0x%08lX age=%lums\n",
                  pc::sysStateName(s.sys), pc::switchName(s.sw), pc::chargeName(s.chr), s.bat_v, s.bat_pct, s.grid_v, s.temp_c,
                  s.vout, s.iout, (unsigned long)s.fault, (unsigned long)age);
    if (s.has_ext) Serial.printf("restarts=%u xfer=0x%02X chg=%.2fA mod=%.4f up=%lus\n", s.restart_count, s.transfer_flags,
                                 s.chg_current, s.mod_index, (unsigned long)s.uptime_s);
  } else if (arg.startsWith("MODE ")) {
    String m = arg.substring(5); m.toLowerCase();
    int mode = m == "off" ? 0 : m == "grid" ? 1 : m == "inverter" ? 2 : m == "auto" ? 3 : -1;
    if (mode < 0) { Serial.println("ERR mode"); return; }
    Serial.println(stm32link::resultName(stm32link::setMode(static_cast<pc::frame::Mode>(mode))));
  } else if (arg == "FAULT_RESET") Serial.println(stm32link::resultName(stm32link::faultReset()));
  else if (arg == "VERSION") {
    uint32_t fw = 0; uint8_t pv = 0;
    auto r = stm32link::getVersion(fw, pv);
    Serial.printf("%s fw=0x%08lX proto=%u\n", stm32link::resultName(r), (unsigned long)fw, pv);
  } else Serial.println("ERR STM32 STATUS|MODE x|FAULT_RESET|VERSION");
}

// STM32 v2.1 read-only text queries (legacy 9600 link) for the station.
void stmText(const char *cmd, const char *prefix) {
  char out[100];
  auto r = stm32link::textQuery(cmd, prefix, out, sizeof out);
  if (r == stm32link::Result::Ok || r == stm32link::Result::Nack) Serial.println(out);
  else Serial.printf("ERR stm32 %s\n", stm32link::resultName(r));
}

// SELFTEST: one JSON line for the factory station.
void selftest() {
  JsonDocument d;
  d["type"] = "selftest";
  d["serial"] = fid::serial().length() ? fid::serial() : String();
  if (!fid::serial().length()) d["serial"] = nullptr;
  d["fw"] = ota::version();
  d["mac"] = settings::id().macColon;
  d["secret"] = fid::hasSecret(); d["secret_reported"] = fid::secretReported();
  d["locked"] = settings::provisioningLocked();
  // JK BMS over the TTL-to-RS485 link (Serial2)
  pc::jk::Data b; uint32_t age = 0;
  bms::Stats bs = bms::stats();
  bool bok = bms::read(b, &age);
  JsonObject jb = d["bms"].to<JsonObject>();
  jb["online"] = bms::online(); jb["frame_valid"] = bok;
  jb["frames"] = bs.frames; jb["ok"] = bs.ok; jb["checksum_errors"] = bs.checksumErrors; jb["bad_frames"] = bs.badFrames; jb["timeouts"] = bs.timeouts;
  if (bok) {
    jb["age_ms"] = age; jb["cells"] = b.cellCount; jb["cell_diff_mv"] = b.cellDiffMv; jb["pack_mv"] = b.packMv; jb["soc"] = b.soc;
    jb["temp1_c"] = b.tempC[0]; jb["temp2_c"] = b.tempC[1]; jb["mos_c"] = b.mosTempC;
  }
  // STM32
  JsonObject js = d["stm32"].to<JsonObject>();
  pc::Stm32Status s; uint32_t sage = 0;
  bool sok = stm32link::status(s, &sage);
  js["online"] = stm32link::online();
  if (sok) { js["age_ms"] = sage; js["fault"] = s.fault; js["bat_v"] = s.bat_v; js["grid_v"] = s.grid_v; js["temp_c"] = s.temp_c; js["vout"] = s.vout; }
  char rep[100];
  if (stm32link::textQuery("VER?", "VER ", rep, sizeof rep) == stm32link::Result::Ok) js["stm32_ver"] = rep + 4; else js["stm32_ver"] = nullptr;
  stm32link::CalInfo cal;
  if (stm32link::textQuery("CAL?", "OK CAL", rep, sizeof rep) == stm32link::Result::Ok && stm32link::parseCal(rep, cal)) {
    js["cal_stored"] = cal.stored; js["cal_active"] = cal.active; js["cal_pending"] = cal.pending; js["cal_scale"] = cal.scale;
  } else { js["cal_stored"] = nullptr; js["cal_active"] = nullptr; js["cal_pending"] = nullptr; js["cal_scale"] = nullptr; }
  // Wi-Fi / heap / cloud
  JsonObject jw = d["wifi"].to<JsonObject>();
  jw["connected"] = WiFi.status() == WL_CONNECTED; jw["state"] = net::stateName(); jw["hotspot"] = net::apActive();
  if (WiFi.status() == WL_CONNECTED) jw["rssi"] = WiFi.RSSI(); else jw["rssi"] = nullptr;
  d["heap_free"] = ESP.getFreeHeap(); d["heap_largest"] = ESP.getMaxAllocHeap();
  JsonObject jc = d["cloud"].to<JsonObject>();
  jc["api_key"] = settings::apiKey().length() > 0; jc["ws_connected"] = backend::connected(); jc["clock_trusted"] = devauth::clockTrusted();
  serializeJson(d, Serial); Serial.println();
}

void handle(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  if (cmd == "INFO") info();
  else if (cmd.startsWith("SET ")) {
    int sp = cmd.indexOf(' ', 4);
    if (sp < 0) { Serial.println("ERR SET <key> <value>"); return; }
    set(cmd.substring(4, sp), cmd.substring(sp + 1));
  } else if (cmd.startsWith("OTA_CONFIG ")) otaConfig(cmd.substring(11));
  else if (cmd == "LOCK") {
    if (!fid::validSerial(fid::serial())) { Serial.println("ERR no serial (SERIAL ACE-NNNNNN first)"); return; }
    if (!fid::secretReported()) { Serial.println("ERR secret not reported to the station yet (SECRET)"); return; }
    bool ok = settings::lock(); Serial.println(ok ? "OK locked" : "ERR");
  } else if (cmd == "SERIAL") { String s = fid::serial(); Serial.printf("SERIAL %s\n", s.length() ? s.c_str() : "unset"); }
  else if (cmd.startsWith("SERIAL ")) {
    if (settings::provisioningLocked()) { Serial.println("ERR locked; FACTORY_WIPE first"); return; }
    String s = cmd.substring(7); s.trim();
    Serial.println(fid::setSerial(s) ? "OK serial set (reboot to apply SSID/ID)" : "ERR serial must be ACE-NNNNNN");
  } else if (cmd == "SECRET") {
    // Factory station only: the secret leaves the unit exactly once, before LOCK.
    char hex[65];
    if (fid::takeSecretOnce(hex)) { Serial.printf("SECRET %s\n", hex); memset(hex, 0, sizeof hex); }
    else Serial.println(fid::hasSecret() ? "ERR already reported (ROTATE_SECRET for a new one)" : "ERR no secret");
  } else if (cmd == "ROTATE_SECRET") Serial.println(fid::rotateSecret() ? "OK new secret; read it with SECRET" : "ERR locked");
  else if (cmd == "SELFTEST") selftest();
  else if (cmd == "STM VER") stmText("VER?", "VER ");
  else if (cmd == "STM CAL?") stmText("CAL?", "OK CAL");
  else if (cmd == "AP ON") { net::forceAp(); Serial.println("OK hotspot on (10 min, or until 120 s after Wi-Fi is up)"); }
  else if (cmd == "PAYMENT_UNLOCK") {
    // Bench only: clear HQ payment_lock without a signed unlock (does not clear cmd ring).
    Serial.println(fid::clearPaymentLock() ? "OK payment unlocked" : "ERR");
  } else if (cmd == "FACTORY_RESET") {
    // v15: as-bought reset (source "button"); keeps the factory identity.
    if (!resetArmedAt || millis() - resetArmedAt > 10000) { resetArmedAt = millis(); Serial.println("Type FACTORY_RESET again within 10 s"); return; }
    resetArmedAt = 0;
    onboarding::factoryReset("button");
    Serial.println("OK factory reset; rebooting");
  } else if (cmd == "FACTORY_WIPE ALL") {
    if (!resetArmedAt || millis() - resetArmedAt > 10000) { resetArmedAt = millis(); Serial.println("Type FACTORY_WIPE ALL again within 10 s (erases serial and secret)"); return; }
    const char *spaces[] = {"pc_secure", "pc_cfg", "pc_offline", "pc_ota", "registry", "forgotten", "pc_onb", "pc_factory", "sock_cfg", "sock_prio", "clock", "pc_ident"};
    for (auto ns : spaces) { Preferences p; if (p.begin(ns, false)) { p.clear(); p.end(); } }
    Serial.println("OK wiped incl. identity; rebooting");
    delay(200); ESP.restart();
  } else if (cmd == "FACTORY_WIPE") {
    // Bench/factory only: erases everything incl. API key and owner (unlocks). The factory
    // identity (serial, secret) survives; "FACTORY_WIPE ALL" erases that too.
    if (!resetArmedAt || millis() - resetArmedAt > 10000) { resetArmedAt = millis(); Serial.println("Type FACTORY_WIPE again within 10 s"); return; }
    const char *spaces[] = {"pc_secure", "pc_cfg", "pc_offline", "pc_ota", "registry", "forgotten", "pc_onb", "pc_factory", "sock_cfg", "sock_prio", "clock"};
    for (auto ns : spaces) { Preferences p; if (p.begin(ns, false)) { p.clear(); p.end(); } }
    Serial.println("OK wiped; rebooting");
    delay(200); ESP.restart();
  }
  else if (cmd.startsWith("REGISTER:")) Serial.println(registerDevice(cmd.substring(9)) ? "pairing" : "Socket not found. Plug it in and try again.");
  else if (cmd.startsWith("UNREGISTER:")) { unregisterDevice(cmd.substring(11)); Serial.println("forget pending"); }
  else if (cmd.startsWith("RELAY:")) {
    int c = cmd.indexOf(':', 6);
    String st = cmd.substring(c + 1);
    if (fid::paymentLocked()) Serial.println("rejected payment_locked");
    else if (c > 6 && st == "ON" && powerguard::latched()) Serial.println("rejected battery_critical_latched");
    else if (c > 6 && (st == "ON" || st == "OFF")) Serial.println(sendRelayCommand(cmd.substring(6, c), st == "ON") ? "queued" : "rejected");
  } else if (cmd.startsWith("PRIO ")) {  // INTERIM: PRIO <socket_id> E|H
    int sp = cmd.indexOf(' ', 5); String pr = cmd.substring(sp + 1);
    if (sp > 5 && (pr == "E" || pr == "H")) { powerguard::setPriority(cmd.substring(5, sp), pr == "E"); Serial.println("OK"); }
    else Serial.println("ERR PRIO <socket_id> E|H");
  } else if (cmd == "POWER_GUARD") Serial.println(powerguard::debugJson());
  else if (cmd == "SOCKETS") Serial.println(buildMetricsJson());
  else if (cmd.startsWith("STM32 ")) stm32(cmd.substring(6));
  else if (cmd == "REBOOT") { ESP.restart(); }
  else Serial.println("ERR unknown command (INFO, SERIAL, SECRET, ROTATE_SECRET, SELFTEST, STM VER, STM CAL?, SET, OTA_CONFIG, LOCK, AP ON, PAYMENT_UNLOCK, FACTORY_RESET, FACTORY_WIPE, REGISTER:, UNREGISTER:, RELAY:, SOCKETS, STM32, REBOOT)");
}
}  // namespace

void service() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!overflow) { line[len] = 0; handle(String(line)); }
      else Serial.println("ERR line too long");
      len = 0; overflow = false;
    } else if (len < sizeof line - 1) line[len++] = c;
    else overflow = true;
  }
}
}  // namespace console
