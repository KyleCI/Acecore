#pragma once
#include <stddef.h>
#include <stdint.h>

// Firmware 2.1 offline-first onboarding (spec /workspace/esp32_review/onboarding_spec.md).
// Pure logic, no Arduino: host-tested. NVS, Wi-Fi and HTTP glue live in src/onboarding.cpp.
namespace pc {
namespace onb {
constexpr uint32_t kPbkdf2Iters = 10000;
constexpr size_t kPasscodeLen = 6;          // decision 2026-10-22: 6 digits, numeric
constexpr size_t kMaxPhones = 8;
constexpr size_t kMaxSessions = 4;
constexpr uint32_t kSessionTtlMs = 1800000;  // 30 min
constexpr uint32_t kClaimTtlS = 900;         // 15 min
constexpr size_t kPhoneIdMax = 40, kPhoneNameMax = 32;

// ---- passcode -----------------------------------------------------------------
// Exactly 6 ASCII digits and not all one digit.
bool validPasscode(const char *p);
struct Verifier { uint8_t salt[16]; uint8_t dk[32]; };  // dk = PBKDF2-HMAC-SHA256(passcode, salt, 10000, 32)
void makeVerifier(const char *passcode, const uint8_t salt[16], Verifier &v);
bool checkPasscode(const char *passcode, const Verifier &v);  // constant-time compare of dk
void verifierHex(const Verifier &v, char out[97]);            // salt(32 hex) + dk(64 hex)
bool verifierFromHex(const char *s, Verifier &v);
bool sameVerifier(const Verifier &a, const Verifier &b);
// Spec section 3/13: first 32 lowercase hex of HMAC-SHA256(factory device_secret, pbkdf2 verifier dk).
void fingerprint(const char *devSecret, const Verifier &v, char out[33]);

// ---- random per-unit values (bytes come from esp_fill_random on target) -------
void passcodeFromRandom(const uint8_t r[8], char out[7]);        // 6 digits, never all one digit
void apPasswordFromRandom(const uint8_t r[12], char out[13]);    // 12 chars, no 0/O/1/l/I
void claimCodeFromRandom(const uint8_t r[5], char out[9]);       // 8 chars RFC 4648 base32
void tokenFromRandom(const uint8_t r[24], char out[49]);         // 48 lowercase hex
// SHA-256 hex of device_id + "|" + claim_code (device_id = 12 upper hex MAC form).
void claimHash(const char *deviceId, const char *code, char out[65]);
// Normalise a typed claim code (upper case, strips spaces/hyphens). False if not 8 base32 chars.
bool normalizeClaimCode(const char *in, char out[9]);

// ---- rate limiter (spec section 5) ----------------------------------------------
// 5 failures, then lockouts of 30 s, 60 s, 120 s ... capped at 15 min. A success resets.
struct Limiter {
  uint16_t fails = 0;
  uint32_t lockedUntilMs = 0;
  bool locked(uint32_t now, uint32_t *retryAfterS = nullptr) const;
  void fail(uint32_t now);
  void success() { fails = 0; lockedUntilMs = 0; }
  static uint32_t lockSeconds(uint16_t fails);  // 0 below 5 failures
};

// ---- paired phones (long-lived LAN tokens, stored only as SHA-256) ----------------
struct Phone {
  uint8_t used;
  char id[kPhoneIdMax + 1];
  char name[kPhoneNameMax + 1];
  uint8_t hash[32];
  uint32_t created, lastSeen;  // unix seconds (0 if the clock is not set)
  uint32_t order;              // monotonic issue counter (eviction without a clock)
};
struct PairStore {
  Phone p[kMaxPhones];
  uint32_t counter;
  void clear();
  // Issue or replace the token for phone_id; evicts the oldest when full. Returns the slot.
  int issue(const char *phoneId, const char *name, const char *token, uint32_t nowUnix);
  int find(const char *token) const;  // slot or -1
  bool remove(const char *phoneId);
  void keepOnly(int slot);  // revoke every other phone (passcode change)
  size_t count() const;
};
static_assert(sizeof(PairStore) < 1400, "PairStore is stored as one NVS blob");

// ---- sessions (RAM only, 30 min) ------------------------------------------------
struct Sessions {
  struct S { uint8_t hash[32]; uint32_t expires; bool used; char phone[kPhoneIdMax + 1]; } s[kMaxSessions] = {};
  void issue(const char *token, uint32_t nowMs, const char *phoneId = "");
  int find(const char *token, uint32_t nowMs) const;  // slot or -1
  bool valid(const char *token, uint32_t nowMs) const { return find(token, nowMs) >= 0; }
  void clear();
};

// ---- Wi-Fi test failure reason (ESP32 disconnect reason codes) -------------------
// 15 4WAY_HANDSHAKE_TIMEOUT, 202 AUTH_FAIL, 204 HANDSHAKE_TIMEOUT, 2 AUTH_EXPIRE -> wrong_password;
// 201 NO_AP_FOUND -> not_found (or 5ghz_only when the app says the phone sees it on 5 GHz).
const char *wifiFailReason(uint8_t reason, bool phoneSees5GhzOnly);

// ---- hotspot policy (spec section 5 "Fallback") -----------------------------------
// ---- Network mode (v14) ------------------------------------------------------------
// Unset = legacy behaviour (unconfigured until a Wi-Fi save). HotspotOnly = AP always on,
// no station, cloud idle. Station = normal router mode.
enum class NetMode : uint8_t { Unset, Station, HotspotOnly };
const char *netModeName(NetMode m);          // "unset" | "station" | "hotspot_only"
bool parseNetMode(const char *s, NetMode &out);  // accepts only "station" | "hotspot_only"
NetMode netModeFromStored(const char *s);     // anything unknown/empty -> Unset

struct ApPolicy {
  uint32_t fallbackMs = 300000;  // AP_FALLBACK_MIN = 5
  uint32_t graceMs = 120000;     // hotspot stays 120 s after STA is up / after wifi save
  uint32_t forceMs = 600000;     // console "AP ON" keeps it at least 10 min
  bool ap = false, wasUp = false, forced = false;
  uint32_t downSince = 0, upSince = 0, forcedUntil = 0;
  void begin(uint32_t now) { downSince = now; }
  // Returns whether the hotspot should be on now.
  bool update(bool hasSsid, bool staUp, bool testing, uint32_t now, bool hotspotOnly = false);
  void force(uint32_t now) { forced = true; forcedUntil = now + forceMs; }
  void restartGrace(uint32_t now) { upSince = now; wasUp = true; }
  static const char *stateName(bool hasSsid, bool staUp, bool testing, bool ap, bool hotspotOnly = false);  // hotspot_only|unconfigured|testing|online|grace|fallback|connecting
};

// ---- WebSocket message rules (section 13) -----------------------------------------
// Error for set_passcode, or nullptr to apply. tls=false -> insecure_transport.
const char *setPasscodeError(const char *requestId, bool tls, uint64_t nowUnix, uint64_t expiresAt, const char *passcode);
bool validUnclaimReason(const char *r);
bool unclaimClearsPairs(const char *r);  // transfer | support clear /v2/pair tokens; owner_request keeps them
bool validOwnerId(const char *o);        // 1..64 printable, no quotes/backslash
}  // namespace onb
}  // namespace pc
