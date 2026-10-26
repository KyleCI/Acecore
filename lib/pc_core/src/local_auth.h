#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure helpers for the local LAN API and the backend contract
// (official-powercell-app PR #3). No Arduino dependencies: host-tested.
namespace pc {
constexpr const char *kLocalUser = "powercell";  // fixed LAN username (backend devices.local_username default)
constexpr size_t kLocalTokenMin = 32;           // backend issues bin2hex(random_bytes(24)) = 48 chars
constexpr size_t kLocalTokenMax = 128;

// Constant-time comparison (length is not secret).
bool ctEquals(const char *a, size_t alen, const char *b, size_t blen);
// Token alphabet [0-9A-Za-z_-], length kLocalTokenMin..kLocalTokenMax.
bool validLocalToken(const char *t);
// RFC 4648 base64 decode (padding optional). False on bad input or overflow; out is NUL-terminated.
bool base64Decode(const char *in, size_t len, char *out, size_t cap, size_t *outLen);
// Parse "Basic <b64(user:pass)>". user/pass are NUL-terminated copies.
bool parseBasic(const char *header, char *user, size_t ucap, char *pass, size_t pcap);
// True if header is Basic powercell:<token> and token matches (constant time).
bool basicMatches(const char *header, const char *token);

// MAC/id equivalence used by the backend: colon, hyphen and bare MACs are one id;
// anything else (legacy names such as ACE4447) compares case-insensitively as-is.
// Writes the canonical form (12 upper hex for a MAC, else the trimmed input).
void canonicalId(const char *in, char *out, size_t cap);
bool sameDeviceId(const char *a, const char *b);

// OTA: the backend returns an absolute url ({APP_URL}/api/ota/download?device_id=...).
// Accept it only if it is on our configured base (same scheme+host+port) and one of the
// documented download paths; or a relative path to one of them. On success writes the
// path+query to fetch relative to base. Rejects '@', '\\', CR/LF, "..".
bool otaDownloadPath(const char *base, const char *url, char *path, size_t cap);
bool isSha256Hex(const char *s);

// Offline battery alert policy for the local /events notifications.
enum class BatteryLevel : uint8_t { None, Low, Critical, ShutdownRisk };
class BatteryAlertPolicy {
 public:
  enum : uint8_t { BatteryChanged = 1, AvailabilityChanged = 2 };
  static constexpr float kLow = 20, kCritical = 10, kShutdown = 5, kHysteresis = 3;
  static constexpr uint32_t kUnavailableAfterMs = 15000;
  uint8_t update(bool valid, float pct, uint32_t nowMs);
  BatteryLevel level() const { return level_; }
  bool available() const { return available_; }
 private:
  BatteryLevel level_ = BatteryLevel::None;
  bool available_ = true;
  uint32_t invalidSince_ = 0;
  bool invalidTiming_ = false;
};
const char *batteryLevelType(BatteryLevel l);  // battery_low | battery_critical | shutdown_imminent
}  // namespace pc
