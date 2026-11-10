#include "devauth.h"
#include <esp_sntp.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include "clock.h"
#include "factory_id.h"
#include "log.h"
#include "settings.h"

namespace devauth {
namespace {
void toHex(const uint8_t *b, size_t n, char *out) { static const char *h = "0123456789abcdef"; for (size_t i = 0; i < n; ++i) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; } out[2 * n] = 0; }
volatile uint32_t g_resyncAt = 0;
}  // namespace

const char *deviceId() {
  static char buf[24];
  String s = fid::serial();
  strlcpy(buf, s.length() ? s.c_str() : settings::id().deviceId, sizeof buf);
  return buf;
}
bool clockTrusted() {
  auto s = clock_::source();
  return time(nullptr) >= 1700000000 && (s == pc::clk::Source::Ntp || s == pc::clk::Source::Phone);
}
bool canSign() { return fid::hasSecret() && clockTrusted(); }

bool sign(const char *method, const char *pathQuery, const uint8_t *body, size_t len, Headers &h) {
  if (!canSign()) return false;
  uint8_t d[32], n[16];
  mbedtls_sha256_ret(body ? body : reinterpret_cast<const uint8_t *>(""), body ? len : 0, d, 0);
  char bodyHex[65]; toHex(d, 32, bodyHex);
  esp_fill_random(n, sizeof n);
  toHex(n, sizeof n, h.nonce);
  snprintf(h.ts, sizeof h.ts, "%lu", (unsigned long)time(nullptr));
  strlcpy(h.id, deviceId(), sizeof h.id);
  String msg; msg.reserve(strlen(method) + strlen(pathQuery) + 120);
  msg += method; msg += '\n'; msg += pathQuery; msg += '\n'; msg += h.ts; msg += '\n'; msg += h.nonce; msg += '\n'; msg += bodyHex;
  uint8_t mac[32];
  if (!fid::hmac(reinterpret_cast<const uint8_t *>(msg.c_str()), msg.length(), mac)) return false;
  toHex(mac, 32, h.sig);
  return true;
}

String pathOf(const String &url) {
  int s = url.indexOf("://");
  int p = url.indexOf('/', s < 0 ? 0 : s + 3);
  return p < 0 ? String("/") : url.substring(p);
}

void resync() {
  g_resyncAt = millis();
  LOGW("AUTH", "TIME_RESYNC", "server reported clock skew; restarting SNTP");
  sntp_restart();
}
bool waitSync(uint32_t ms) {
  uint32_t t0 = millis(), before = clock_::ntpAtMs();
  while (millis() - t0 < ms) {
    uint32_t a = clock_::ntpAtMs();
    if (a && a != before) return true;
    delay(100);
  }
  return false;
}
}  // namespace devauth
