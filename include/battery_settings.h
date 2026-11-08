#pragma once

#include <stdint.h>

namespace powercell {

// Direct-local battery settings for this collector source.
// Hard cutoff (<=10) and restart eligibility (>=15) are not writable.
// KyleCI/Acecore does not issue a per-phone LAN bearer token and has no /v2/login.
enum class SettingsAuth : uint8_t {
  Ok = 0,
  Missing,
  Malformed,
  LanTokenUnsupported
};

enum class SettingsReason : uint8_t {
  None = 0,
  Unauthorized,
  LanTokenUnsupported,
  UnsupportedSchema,
  NotInteger,
  OutOfRange,
  WarningNotAboveCutoff,
  LowNotAboveWarning,
  StaleRevision,
  PersistFailed
};

struct SettingsState {
  int schema = 1;
  int low_percent = 25;
  int warning_percent = 15;
  int inhibit_percent = 10;
  int restart_percent = 15;
  uint32_t applied_revision = 0;
  bool durable = true;
  bool physical_shutdown_verified = false;
};

struct SettingsDecision {
  bool applied = false;
  bool durable_commit = false;
  bool pending = false;
  SettingsReason reason = SettingsReason::None;
  SettingsState readback;
};

// This firmware never returns Ok. A non-empty Bearer token is recognized and rejected.
SettingsAuth classify_lan_authorization(bool has_header, bool bearer_scheme, bool token_nonempty);

class BatterySettings {
 public:
  static constexpr int kSchema = 1;
  static constexpr int kInhibit = 10;
  static constexpr int kRestart = 15;
  static constexpr int kWarningMin = 11;
  static constexpr int kWarningMax = 80;
  static constexpr int kLowMin = 12;
  static constexpr int kLowMax = 95;

  SettingsDecision readback() const { return decide(false, SettingsReason::None, state_); }
  void load(int low, int warning, uint32_t revision);
  SettingsDecision set(SettingsAuth auth, int schema, bool low_is_int, int low,
                       bool warning_is_int, int warning, uint32_t base_revision, bool persist_ok);
  const SettingsState& state() const { return state_; }

 private:
  static SettingsDecision decide(bool applied, SettingsReason reason, const SettingsState& state);
  SettingsState state_;
};

const char* settings_reason_name(SettingsReason reason);

}  // namespace powercell
