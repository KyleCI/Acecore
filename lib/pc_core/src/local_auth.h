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

// Offline battery alert policy for the local /events notifications and the cloud
// battery_soc event (events_core). Caleb 8 Nov 2026 (include/battery_alert_policy.h):
//   low       <= 25 %  battery_low
//   critical  <= 15 %  battery_critical, the one-time shutdown warning
//   shutdown  <= 10 %  output off (power_guard latch, battery_critical event); restart blocked until 15 %
// The low and critical percents follow the saved Manual battery settings
// (/v1/battery-settings: low_percent, warning_percent) when valid, else 25/15.
// 2.5.13: the 5 % shutdown_imminent level is gone. BatteryLevel::ShutdownRisk is
// kept for the enum layout but this policy never returns it.
// Worsening applies at once. Low clears above low + 3. Critical is one notification
// per episode: it stays while the output latch holds and clears only after the
// SoC stays above critical + 3 for 60 s, so a 15/16 % wobble does not repeat it.
enum class BatteryLevel : uint8_t { None, Low, Critical, ShutdownRisk };
class BatteryAlertPolicy {
 public:
  enum : uint8_t { BatteryChanged = 1, AvailabilityChanged = 2 };
  static constexpr float kLow = 25, kCritical = 15, kShutdown = 10, kRestart = 15, kHysteresis = 3;
  static constexpr uint32_t kUnavailableAfterMs = 15000;
  static constexpr uint32_t kCriticalClearHoldMs = 60000;
  // Same bounds as BatterySettings (include/battery_settings.h): warning 11..80, low 12..95, low > warning.
  static bool validThresholds(float low, float critical) {
    return low == low && critical == critical && critical >= 11 && critical <= 80 && low >= 12 && low <= 95 && low > critical;
  }
  // Returns false and keeps 25/15 when the pair is not valid.
  bool setThresholds(float low, float critical);
  float lowPercent() const { return low_; }
  float criticalPercent() const { return crit_; }
  // latched: the output is held off by the battery latch (power_guard). Keeps Critical.
  uint8_t update(bool valid, float pct, uint32_t nowMs, bool latched = false);
  BatteryLevel level() const { return level_; }
  bool available() const { return available_; }
 private:
  BatteryLevel levelAt(float p) const;
  BatteryLevel level_ = BatteryLevel::None;
  bool available_ = true;
  uint32_t invalidSince_ = 0;
  bool invalidTiming_ = false;
  bool clearHold_ = false;
  uint32_t clearSince_ = 0;
  float low_ = kLow, crit_ = kCritical;
};
// battery_low | battery_critical (| shutdown_imminent, no longer produced)
const char *batteryLevelType(BatteryLevel l);
}  // namespace pc
