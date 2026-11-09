#include "local_auth.h"
#include <ctype.h>
#include <string.h>

namespace pc {
bool ctEquals(const char *a, size_t alen, const char *b, size_t blen) {
  if (!a || !b || alen != blen) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < alen; ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

bool validLocalToken(const char *t) {
  if (!t) return false;
  size_t n = strlen(t);
  if (n < kLocalTokenMin || n > kLocalTokenMax) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = t[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
  }
  return true;
}

static int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64Decode(const char *in, size_t len, char *out, size_t cap, size_t *outLen) {
  if (!in || !out || !cap) return false;
  while (len && in[len - 1] == '=') --len;
  if (len % 4 == 1) return false;
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; ++i) {
    int v = b64v(in[i]);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o + 1 >= cap) return false;
      out[o++] = static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  out[o] = 0;
  if (outLen) *outLen = o;
  return true;
}

bool parseBasic(const char *h, char *user, size_t ucap, char *pass, size_t pcap) {
  if (!h || !user || !pass || !ucap || !pcap) return false;
  while (*h == ' ') ++h;
  if (strncasecmp(h, "Basic ", 6) != 0) return false;
  h += 6;
  while (*h == ' ') ++h;
  size_t len = strlen(h);
  while (len && (h[len - 1] == ' ' || h[len - 1] == '\r' || h[len - 1] == '\n')) --len;
  if (!len || len > 400) return false;
  char buf[304];
  size_t n = 0;
  if (!base64Decode(h, len, buf, sizeof buf, &n)) return false;
  if (memchr(buf, 0, n)) return false;
  char *colon = static_cast<char *>(memchr(buf, ':', n));
  if (!colon) return false;
  size_t ul = static_cast<size_t>(colon - buf), pl = n - ul - 1;
  if (ul >= ucap || pl >= pcap) return false;
  memcpy(user, buf, ul); user[ul] = 0;
  memcpy(pass, colon + 1, pl); pass[pl] = 0;
  return true;
}

bool basicMatches(const char *header, const char *token) {
  if (!token || !validLocalToken(token)) return false;
  char u[24], p[kLocalTokenMax + 2];
  if (!parseBasic(header, u, sizeof u, p, sizeof p)) return false;
  bool userOk = strcmp(u, kLocalUser) == 0;
  bool passOk = ctEquals(p, strlen(p), token, strlen(token));
  return userOk && passOk;
}

static bool macCanon(const char *s, char out[13]) {
  size_t n = strlen(s), o = 0;
  if (n == 12) {
    for (size_t i = 0; i < 12; ++i) { if (!isxdigit(static_cast<unsigned char>(s[i]))) return false; out[o++] = toupper(s[i]); }
  } else if (n == 17) {
    char sep = s[2];
    if (sep != ':' && sep != '-') return false;
    for (size_t i = 0; i < 17; ++i) {
      if (i % 3 == 2) { if (s[i] != sep) return false; continue; }
      if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
      out[o++] = toupper(s[i]);
    }
  } else return false;
  out[12] = 0;
  return true;
}

void canonicalId(const char *in, char *out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (!in) return;
  while (*in == ' ') ++in;
  char tmp[64];
  size_t n = strlen(in);
  while (n && in[n - 1] == ' ') --n;
  if (n >= sizeof tmp) n = sizeof tmp - 1;
  memcpy(tmp, in, n); tmp[n] = 0;
  char mac[13];
  const char *src = macCanon(tmp, mac) ? mac : tmp;
  strncpy(out, src, cap - 1);
  out[cap - 1] = 0;
}

bool sameDeviceId(const char *a, const char *b) {
  char x[64], y[64];
  canonicalId(a, x, sizeof x); canonicalId(b, y, sizeof y);
  return x[0] && strcasecmp(x, y) == 0;
}

bool isSha256Hex(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; ++i) if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

static bool allowedOtaPath(const char *p) {
  static const char *kPaths[] = {"/api/ota/download", "/api/firmware/download", "/api/firmware.bin"};
  for (const char *k : kPaths) {
    size_t n = strlen(k);
    if (strncmp(p, k, n) == 0 && (p[n] == 0 || p[n] == '?')) return true;
  }
  return false;
}

bool otaDownloadPath(const char *base, const char *url, char *path, size_t cap) {
  if (!base || !url || !path || !cap) return false;
  if (strchr(url, '@') || strchr(url, '\\') || strchr(url, '\r') || strchr(url, '\n') || strchr(url, ' ') || strstr(url, "..")) return false;
  size_t bl = strlen(base);
  while (bl && base[bl - 1] == '/') --bl;
  const char *rel = nullptr;
  if (url[0] == '/') rel = url;
  else if (bl && strncasecmp(url, base, bl) == 0 && url[bl] == '/') rel = url + bl;
  else return false;
  if (!allowedOtaPath(rel) || strlen(rel) >= cap) return false;
  strcpy(path, rel);
  return true;
}

static BatteryLevel levelAt(float p) {
  return p <= BatteryAlertPolicy::kShutdown ? BatteryLevel::ShutdownRisk : p <= BatteryAlertPolicy::kCritical ? BatteryLevel::Critical
       : p <= BatteryAlertPolicy::kLow ? BatteryLevel::Low : BatteryLevel::None;
}

uint8_t BatteryAlertPolicy::update(bool valid, float pct, uint32_t now) {
  uint8_t changes = 0;
  if (valid) {
    invalidTiming_ = false;
    if (!available_) { available_ = true; changes |= AvailabilityChanged; }
    BatteryLevel raw = levelAt(pct);
    // Worsening applies at once; recovering needs kHysteresis percent of margin.
    BatteryLevel next = raw >= level_ ? raw : (levelAt(pct - kHysteresis) > raw ? levelAt(pct - kHysteresis) : raw);
    if (next != level_) { level_ = next; changes |= BatteryChanged; }
  } else {
    if (!invalidTiming_) { invalidTiming_ = true; invalidSince_ = now; }
    if (available_ && now - invalidSince_ >= kUnavailableAfterMs) {
      available_ = false;
      changes |= AvailabilityChanged;
      if (level_ != BatteryLevel::None) { level_ = BatteryLevel::None; changes |= BatteryChanged; }
    }
  }
  return changes;
}

const char *batteryLevelType(BatteryLevel l) {
  switch (l) {
    case BatteryLevel::ShutdownRisk: return "shutdown_imminent";
    case BatteryLevel::Critical: return "battery_critical";
    default: return "battery_low";
  }
}
}  // namespace pc
