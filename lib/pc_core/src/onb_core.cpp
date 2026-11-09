#include "onb_core.h"
#include <string.h>
#include "sha256.h"

namespace pc {
namespace onb {
namespace {
bool ctEq(const uint8_t *a, const uint8_t *b, size_t n) { uint8_t d = 0; for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i]; return d == 0; }
uint64_t le64(const uint8_t *r) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | r[i]; return v; }
}  // namespace

bool validPasscode(const char *p) {
  if (!p || strlen(p) != kPasscodeLen) return false;
  bool same = true;
  for (size_t i = 0; i < kPasscodeLen; ++i) { if (p[i] < '0' || p[i] > '9') return false; if (p[i] != p[0]) same = false; }
  return !same;
}
void makeVerifier(const char *pc_, const uint8_t salt[16], Verifier &v) {
  memcpy(v.salt, salt, 16);
  pbkdf2Sha256(reinterpret_cast<const uint8_t *>(pc_), strlen(pc_), v.salt, 16, kPbkdf2Iters, v.dk, 32);
}
bool checkPasscode(const char *p, const Verifier &v) {
  if (!p || !*p || strlen(p) > 64) return false;
  Verifier t; makeVerifier(p, v.salt, t);
  return ctEq(t.dk, v.dk, 32);
}
void verifierHex(const Verifier &v, char out[97]) { toHex(v.salt, 16, out); toHex(v.dk, 32, out + 32); }
bool verifierFromHex(const char *s, Verifier &v) {
  if (!s || strlen(s) != 96) return false;
  char a[33]; memcpy(a, s, 32); a[32] = 0;
  return fromHex(a, v.salt, 16) && fromHex(s + 32, v.dk, 32);
}
bool sameVerifier(const Verifier &a, const Verifier &b) { return ctEq(a.salt, b.salt, 16) && ctEq(a.dk, b.dk, 32); }
void fingerprint(const char *secret, const Verifier &v, char out[33]) {
  uint8_t m[32];
  hmacSha256(reinterpret_cast<const uint8_t *>(secret), strlen(secret), v.dk, 32, m);
  toHex(m, 16, out);
}

void passcodeFromRandom(const uint8_t r[8], char out[7]) {
  uint64_t x = le64(r) % 1000000ULL;  // bias < 2^-44
  for (int i = 5; i >= 0; --i) { out[i] = char('0' + x % 10); x /= 10; }
  out[6] = 0;
  if (!validPasscode(out)) out[5] = out[5] == '9' ? '8' : char(out[5] + 1);  // all-one-digit (p = 1e-5)
}
void apPasswordFromRandom(const uint8_t r[12], char out[13]) {
  static const char a[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // 56: no 0 O 1 l I
  for (int i = 0; i < 12; ++i) out[i] = a[r[i] % (sizeof a - 1)];
  out[12] = 0;
}
void claimCodeFromRandom(const uint8_t r[5], char out[9]) {
  static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  uint64_t x = 0; for (int i = 0; i < 5; ++i) x = x << 8 | r[i];
  for (int i = 7; i >= 0; --i) { out[i] = a[x & 31]; x >>= 5; }
  out[8] = 0;
}
void tokenFromRandom(const uint8_t r[24], char out[49]) { toHex(r, 24, out); }
void claimHash(const char *deviceId, const char *code, char out[65]) {
  Sha256 s; s.update(deviceId, strlen(deviceId)); s.update("|", 1); s.update(code, strlen(code));
  uint8_t d[32]; s.finish(d); toHex(d, 32, out);
}
bool normalizeClaimCode(const char *in, char out[9]) {
  size_t n = 0;
  for (; in && *in; ++in) {
    char c = *in;
    if (c == ' ' || c == '-') continue;
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7')) || n >= 8) return false;
    out[n++] = c;
  }
  out[n] = 0;
  return n == 8;
}

uint32_t Limiter::lockSeconds(uint16_t f) {
  if (f < 5) return 0;
  uint32_t s = 30; for (uint16_t i = 5; i < f && s < 900; ++i) s *= 2;
  return s > 900 ? 900 : s;
}
bool Limiter::locked(uint32_t now, uint32_t *ra) const {
  if (!lockedUntilMs || (int32_t)(now - lockedUntilMs) >= 0) return false;
  if (ra) *ra = (lockedUntilMs - now + 999) / 1000;
  return true;
}
void Limiter::fail(uint32_t now) {
  if (fails < 1000) ++fails;
  uint32_t s = lockSeconds(fails);
  if (s) { lockedUntilMs = now + s * 1000; if (!lockedUntilMs) lockedUntilMs = 1; }
}

void PairStore::clear() { memset(this, 0, sizeof *this); }
int PairStore::issue(const char *id, const char *name, const char *token, uint32_t now) {
  int slot = -1;
  for (size_t i = 0; i < kMaxPhones; ++i) if (p[i].used && !strcmp(p[i].id, id)) slot = int(i);
  if (slot < 0) for (size_t i = 0; i < kMaxPhones; ++i) if (!p[i].used) { slot = int(i); break; }
  if (slot < 0) { slot = 0; for (size_t i = 1; i < kMaxPhones; ++i) if (p[i].order < p[slot].order) slot = int(i); }
  Phone &x = p[slot];
  bool same = x.used && !strcmp(x.id, id);
  uint32_t created = same ? x.created : now;
  memset(&x, 0, sizeof x);
  x.used = 1;
  strncpy(x.id, id, kPhoneIdMax);
  strncpy(x.name, name ? name : "", kPhoneNameMax);
  sha256(token, strlen(token), x.hash);
  x.created = created; x.lastSeen = now; x.order = ++counter;
  return slot;
}
int PairStore::find(const char *token) const {
  if (!token || !*token) return -1;
  uint8_t h[32]; sha256(token, strlen(token), h);
  int hit = -1;
  for (size_t i = 0; i < kMaxPhones; ++i) if (p[i].used && ctEq(p[i].hash, h, 32)) hit = int(i);
  return hit;
}
bool PairStore::remove(const char *id) {
  for (auto &x : p) if (x.used && !strcmp(x.id, id)) { memset(&x, 0, sizeof x); return true; }
  return false;
}
void PairStore::keepOnly(int slot) { for (int i = 0; i < int(kMaxPhones); ++i) if (i != slot) memset(&p[i], 0, sizeof p[i]); }
size_t PairStore::count() const { size_t n = 0; for (auto &x : p) n += x.used ? 1 : 0; return n; }

void Sessions::issue(const char *t, uint32_t now, const char *phone) {
  int slot = 0;
  for (int i = 0; i < int(kMaxSessions); ++i) {
    if (!s[i].used || (int32_t)(now - s[i].expires) >= 0) { slot = i; break; }
    if ((int32_t)(s[i].expires - s[slot].expires) < 0) slot = i;
  }
  sha256(t, strlen(t), s[slot].hash);
  s[slot].expires = now + kSessionTtlMs; s[slot].used = true;
  memset(s[slot].phone, 0, sizeof s[slot].phone);
  strncpy(s[slot].phone, phone ? phone : "", kPhoneIdMax);
}
int Sessions::find(const char *t, uint32_t now) const {
  if (!t || !*t) return -1;
  uint8_t h[32]; sha256(t, strlen(t), h);
  int hit = -1;
  for (int i = 0; i < int(kMaxSessions); ++i)
    if (s[i].used && (int32_t)(now - s[i].expires) < 0 && ctEq(s[i].hash, h, 32)) hit = i;
  return hit;
}
void Sessions::clear() { memset(s, 0, sizeof s); }

const char *wifiFailReason(uint8_t r, bool fiveG) {
  switch (r) {
    case 2: case 15: case 202: case 204: return "wrong_password";
    case 201: return fiveG ? "5ghz_only" : "not_found";
    default: return nullptr;
  }
}

const char *netModeName(NetMode m) { return m == NetMode::Station ? "station" : m == NetMode::HotspotOnly ? "hotspot_only" : "unset"; }
bool parseNetMode(const char *s, NetMode &out) {
  if (!s) return false;
  if (!strcmp(s, "station")) { out = NetMode::Station; return true; }
  if (!strcmp(s, "hotspot_only")) { out = NetMode::HotspotOnly; return true; }
  return false;
}
NetMode netModeFromStored(const char *s) { NetMode m = NetMode::Unset; return parseNetMode(s, m) ? m : NetMode::Unset; }

bool ApPolicy::update(bool hasSsid, bool staUp, bool testing, uint32_t now, bool hotspotOnly) {
  if (staUp && !wasUp) upSince = now;
  if (!staUp && wasUp) downSince = now;
  wasUp = staUp;
  if (forced && (int32_t)(now - forcedUntil) >= 0) forced = false;
  if (hotspotOnly || !hasSsid || testing || forced) ap = true;
  else if (!staUp) ap = ap || now - downSince >= fallbackMs;
  else ap = ap && now - upSince < graceMs;
  return ap;
}
const char *ApPolicy::stateName(bool hasSsid, bool staUp, bool testing, bool ap, bool hotspotOnly) {
  if (testing) return "testing";
  if (hotspotOnly) return "hotspot_only";
  if (!hasSsid) return "unconfigured";
  if (staUp && !ap) return "online";
  if (staUp && ap) return "grace";  // v12: STA up, hotspot still in its 120 s grace (was mislabeled "fallback")
  if (ap) return "fallback";
  return "connecting";
}

const char *setPasscodeError(const char *rid, bool tls, uint64_t now, uint64_t exp, const char *p) {
  size_t n = rid ? strlen(rid) : 0;
  if (!n || n > 47) return "invalid_request_id";
  if (!tls) return "insecure_transport";
  if (exp && now && exp <= now) return "expired";
  if (!validPasscode(p)) return "invalid_passcode";
  return nullptr;
}
bool validUnclaimReason(const char *r) { return r && (!strcmp(r, "owner_request") || !strcmp(r, "transfer") || !strcmp(r, "support")); }
bool unclaimClearsPairs(const char *r) { return r && (!strcmp(r, "transfer") || !strcmp(r, "support")); }
bool validOwnerId(const char *o) {
  size_t n = o ? strlen(o) : 0;
  if (!n || n > 64) return false;
  for (; *o; ++o) if (*o < 0x21 || *o > 0x7e || *o == '"' || *o == '\\') return false;
  return true;
}
}  // namespace onb
}  // namespace pc
