#pragma once

#include "lan_access.h"

#include <stdint.h>

namespace powercell {

// Direct-local battery settings for this collector source.
// Hard cutoff (<=10) and restart eligibility (>=15) are not writable.
// Device login is POST /v2/login. Manual Save uses the returned lan_token.
// classify_lan_authorization is the only settings gate. Ok means the header is
// Bearer and the token store says it is a saved LAN token.
enum class SettingsAuth : uint8_t {
  Ok = 0,
  Missing,
  Malformed,
  SessionOnly,
  Rejected,
  LanTokenUnsupported
};

enum class SettingsReason : uint8_t {
  None = 0,
  Unauthorized,
  SessionNotSufficient,
  LanTokenUnsupported,
  UnsupportedSchema,
  NotInteger,
  OutOfRange,
  WarningNotAboveCutoff,
  LowNotAboveWarning,
  StaleRevision,
  PersistFailed,
  UnknownRating,
  CeilingAboveUsable,
  UsableUnset,
  FactoryProvisioned
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

// Ok only when authorization is "Bearer <saved lan_token>" and kind is LanKind::Lan.
// A login session, API-KEY, or OTA bearer is not Ok.
SettingsAuth classify_lan_authorization(const char* authorization, LanKind kind);

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
