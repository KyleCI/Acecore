#include "prefs_safe.h"
#include "factory_id.h"
#include <Preferences.h>
#include <esp_system.h>
#include <bootloader_random.h>
#include <mbedtls/md.h>
#include "log.h"
#include "settings.h"

namespace fid {
namespace {
constexpr const char *NS = "pc_ident";
constexpr int kCmdRing = 8;
struct CmdRing { char id[kCmdRing][65]; uint8_t next; };
uint8_t g_secret[32];
bool g_hasSecret = false, g_reported = false;
String g_serial, g_ap, g_payReason;
bool g_payLocked = false;
uint32_t g_payLast = 0;
SemaphoreHandle_t g_mx = nullptr;
struct Lock { Lock() { xSemaphoreTakeRecursive(g_mx, portMAX_DELAY); } ~Lock() { xSemaphoreGiveRecursive(g_mx); } };

void toHex(const uint8_t *b, size_t n, char *out) { static const char *h = "0123456789abcdef"; for (size_t i = 0; i < n; ++i) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; } out[2 * n] = 0; }
bool fromHex(const String &s, uint8_t *out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; ++i) {
    char b[3] = {s[2 * i], s[2 * i + 1], 0};
    if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return false;
    out[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  return true;
}
bool storeSecret(const uint8_t s[32], bool reported) {
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = p.putBytes("secret", s, 32) == 32 && p.putBool("sec_rep", reported);
  p.end();
  if (ok) { memcpy(g_secret, s, 32); g_hasSecret = true; g_reported = reported; }
  return ok;
}
}  // namespace

void begin() {
  if (!g_mx) g_mx = xSemaphoreCreateRecursiveMutex();
  Lock l;
  Preferences p;
  if (prefsOpenRO(p, NS)) {
    g_serial = prefsStr(p, "serial", "");
    g_ap = prefsStr(p, "ap_pass", "");
    g_hasSecret = prefsLen(p, "secret") == 32 && p.getBytes("secret", g_secret, 32) == 32;
    g_reported = p.getBool("sec_rep", false);
    g_payLocked = p.getBool("pay_lock", false);
    g_payReason = prefsStr(p, "pay_rsn", "");
    g_payLast = p.getUInt("pay_last", 0);
    p.end();
  }
  if (!validSerial(g_serial)) g_serial = "";
  if (g_ap.length() < 8) {
    // Migrate the label hotspot password from v14 (pc_factory/ap_pass, else pc_secure/ap_pass).
    String ap;
    if (prefsOpenRO(p, "pc_factory")) { ap = prefsStr(p, "ap_pass", ""); p.end(); }
    if (ap.length() < 8) ap = settings::getString("ap_pass");
    if (ap.length() >= 8 && setApPass(ap)) LOGI("FID", "AP_PASS_MIGRATED", "label hotspot password moved to the factory identity");
  }
  if (!g_hasSecret) {
    // Bench/field units provisioned with SET device_secret keep that secret (backend already has it).
    // v17: also absorb a leftover NVS "dev_secret" (passcode fingerprint key) when it is 64 hex.
    String legacy = settings::getString("device_secret");
    uint8_t s[32];
    if (settings::validCloudSecret(legacy) && fromHex(legacy, s, 32)) {
      if (storeSecret(s, true)) { settings::setString("device_secret", ""); LOGI("FID", "SECRET_MIGRATED", "existing device_secret moved to the factory identity (len=32)"); }
    } else {
      String oldDev = settings::getString("dev_secret");
      if (settings::validCloudSecret(oldDev) && fromHex(oldDev, s, 32)) {
        if (storeSecret(s, true)) {
          settings::setString("dev_secret", "");
          LOGI("FID", "DEV_SECRET_MIGRATED", "legacy NVS dev_secret moved to the factory identity (len=32)");
        }
      } else {
        bootloader_random_enable();  // Wi-Fi is not up yet: enable the SAR ADC entropy source
        esp_fill_random(s, sizeof s);
        bootloader_random_disable();
        if (storeSecret(s, false)) LOGI("FID", "SECRET_GENERATED", "32 bytes, not yet reported (console SECRET, once)");
      }
    }
    memset(s, 0, sizeof s);
  }
  // v17: factory identity is the only HMAC key. Drop leftover NVS "dev_secret" when pc_ident is set
  // (case both keys, or after migrate). Never log the value.
  if (g_hasSecret && settings::getString("dev_secret").length()) {
    settings::setString("dev_secret", "");
    LOGI("FID", "DEV_SECRET_CLEARED", "legacy NVS dev_secret dropped; using factory identity secret");
  }
  LOGI("FID", "IDENTITY", "serial=%s secret=%s reported=%d ap_pass=%s payment_locked=%d", g_serial.length() ? g_serial.c_str() : "unset",
       g_hasSecret ? "set" : "MISSING", g_reported, g_ap.length() ? "set" : "unset", (int)g_payLocked);
}

bool validSerial(const String &s) {
  if (s.length() != 10 || !s.startsWith("ACE-")) return false;
  for (int i = 4; i < 10; ++i) if (!isdigit((unsigned char)s[i])) return false;
  return true;
}
String serial() { Lock l; return g_serial; }
String serialDigits() { Lock l; return g_serial.length() ? g_serial.substring(4) : String(); }
bool setSerial(const String &s) {
  if (!validSerial(s)) return false;
  Lock l;
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = p.putString("serial", s) == s.length();
  p.end();
  if (ok) g_serial = s;
  return ok;
}

bool hasSecret() { Lock l; return g_hasSecret; }
bool secretReported() { Lock l; return g_reported; }
bool takeSecretOnce(char hex[65]) {
  Lock l;
  if (!g_hasSecret || g_reported || settings::provisioningLocked()) return false;
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = p.putBool("sec_rep", true);  // mark first: a crash after printing never prints twice
  p.end();
  if (!ok) return false;
  g_reported = true;
  toHex(g_secret, 32, hex);
  return true;
}
bool setSecretHex(const String &h) {
  uint8_t s[32];
  String v = h; v.trim(); v.toLowerCase();
  if (!fromHex(v, s, 32)) return false;
  Lock l;
  bool ok = storeSecret(s, true);  // the station supplied it, so it already knows it
  memset(s, 0, sizeof s);
  return ok;
}
bool rotateSecret() {
  if (settings::provisioningLocked()) return false;
  uint8_t s[32];
  bootloader_random_enable(); esp_fill_random(s, sizeof s); bootloader_random_disable();
  Lock l;
  bool ok = storeSecret(s, false);
  memset(s, 0, sizeof s);
  return ok;
}
String legacySecretHex() { Lock l; if (!g_hasSecret) return ""; char h[65]; toHex(g_secret, 32, h); String r = h; memset(h, 0, sizeof h); return r; }
bool hmac(const uint8_t *msg, size_t n, uint8_t out[32]) {
  Lock l;
  if (!g_hasSecret) return false;
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), g_secret, 32, msg, n, out) == 0;
}

String apPass() { Lock l; return g_ap; }
bool setApPass(const String &a) {
  if (a.length() < 8 || a.length() > 63) return false;
  Lock l;
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = p.putString("ap_pass", a) == a.length();
  p.end();
  if (ok) g_ap = a;
  return ok;
}

void markReset(const char *source, const String &cmdId, uint32_t resetAt, uint32_t lastAt) {
  Lock l;
  Preferences p; if (!p.begin(NS, false)) return;
  p.putBool("rst_pend", true);
  p.putString("rst_src", source);
  if (cmdId.length()) p.putString("rst_cmd", cmdId); else if (p.isKey("rst_cmd")) p.remove("rst_cmd");
  p.putUInt("rst_at", resetAt);
  if (lastAt > p.getUInt("rst_last", 0)) p.putUInt("rst_last", lastAt);
  if (cmdId.length() && cmdId.length() <= 64) {
    CmdRing r{};
    if (prefsLen(p, "rst_ids") == sizeof r) p.getBytes("rst_ids", &r, sizeof r);
    strlcpy(r.id[r.next % kCmdRing], cmdId.c_str(), 65); r.next = (r.next + 1) % kCmdRing;
    p.putBytes("rst_ids", &r, sizeof r);
  }
  p.end();
}
ResetInfo resetInfo() {
  Lock l;
  ResetInfo r; Preferences p;
  if (prefsOpenRO(p, NS)) {
    r.pending = p.getBool("rst_pend", false);
    r.source = prefsStr(p, "rst_src", "button"); r.cmdId = prefsStr(p, "rst_cmd", ""); r.at = p.getUInt("rst_at", 0);
    p.end();
  }
  return r;
}
void clearResetPending() { Lock l; Preferences p; if (p.begin(NS, false)) { p.putBool("rst_pend", false); p.end(); } }
uint32_t lastResetAt() { Lock l; Preferences p; uint32_t v = 0; if (prefsOpenRO(p, NS)) { v = p.getUInt("rst_last", 0); p.end(); } return v; }
bool cmdSeen(const String &id) {
  Lock l;
  Preferences p; CmdRing r{}; bool have = false;
  if (prefsOpenRO(p, NS)) { have = prefsLen(p, "rst_ids") == sizeof r && p.getBytes("rst_ids", &r, sizeof r) == sizeof r; p.end(); }
  if (!have) return false;
  for (auto &x : r.id) { x[64] = 0; if (x[0] && id == x) return true; }
  return false;
}

bool paymentLocked() { Lock l; return g_payLocked; }
String paymentLockReason() { Lock l; return g_payReason; }
uint32_t lastPaymentLockAt() { Lock l; return g_payLast; }
bool paymentCmdSeen(const String &id) {
  Lock l;
  Preferences p; CmdRing r{}; bool have = false;
  if (prefsOpenRO(p, NS)) { have = prefsLen(p, "pay_ids") == sizeof r && p.getBytes("pay_ids", &r, sizeof r) == sizeof r; p.end(); }
  if (!have) return false;
  for (auto &x : r.id) { x[64] = 0; if (x[0] && id == x) return true; }
  return false;
}
bool applyPaymentLock(bool locked, const String &reason, const String &cmdId, uint32_t issuedAt) {
  if (!issuedAt || cmdId.length() == 0 || cmdId.length() > 64 || reason.length() > 64) return false;
  Lock l;
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = p.putBool("pay_lock", locked);
  if (locked) {
    ok = ok && p.putString("pay_rsn", reason) == reason.length();
  } else {
    if (p.isKey("pay_rsn")) p.remove("pay_rsn");
  }
  if (issuedAt > p.getUInt("pay_last", 0)) ok = ok && p.putUInt("pay_last", issuedAt);
  CmdRing r{};
  if (prefsLen(p, "pay_ids") == sizeof r) p.getBytes("pay_ids", &r, sizeof r);
  strlcpy(r.id[r.next % kCmdRing], cmdId.c_str(), 65); r.next = (r.next + 1) % kCmdRing;
  ok = ok && p.putBytes("pay_ids", &r, sizeof r) == sizeof r;
  p.end();
  if (ok) {
    g_payLocked = locked;
    g_payReason = locked ? reason : String();
    if (issuedAt > g_payLast) g_payLast = issuedAt;
  }
  return ok;
}
bool clearPaymentLock() {
  Lock l;
  Preferences p; if (!p.begin(NS, false)) return false;
  bool ok = p.putBool("pay_lock", false);
  if (p.isKey("pay_rsn")) p.remove("pay_rsn");
  p.end();
  if (ok) { g_payLocked = false; g_payReason = ""; }
  return ok;
}
}  // namespace fid
