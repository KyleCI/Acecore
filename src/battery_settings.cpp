#include "battery_settings.h"

namespace powercell {

SettingsAuth classify_lan_authorization(bool has_header, bool bearer_scheme, bool token_nonempty) {
  if (!has_header || !bearer_scheme || !token_nonempty) return SettingsAuth::Missing;
  return SettingsAuth::LanTokenUnsupported;
}

const char* settings_reason_name(SettingsReason reason) {
  switch (reason) {
    case SettingsReason::None: return "none";
    case SettingsReason::Unauthorized: return "unauthorized";
    case SettingsReason::LanTokenUnsupported: return "lan_token_unsupported";
    case SettingsReason::UnsupportedSchema: return "unsupported_schema";
    case SettingsReason::NotInteger: return "not_integer";
    case SettingsReason::OutOfRange: return "out_of_range";
    case SettingsReason::WarningNotAboveCutoff: return "warning_not_above_cutoff";
    case SettingsReason::LowNotAboveWarning: return "low_not_above_warning";
    case SettingsReason::StaleRevision: return "stale_revision";
    case SettingsReason::PersistFailed: return "persist_failed";
  }
  return "unknown";
}

SettingsDecision BatterySettings::decide(bool applied, SettingsReason reason, const SettingsState& state) {
  SettingsDecision out;
  out.applied = applied;
  out.durable_commit = applied && state.durable;
  out.pending = false;
  out.reason = reason;
  out.readback = state;
  out.readback.inhibit_percent = kInhibit;
  out.readback.restart_percent = kRestart;
  out.readback.schema = kSchema;
  out.readback.physical_shutdown_verified = false;
  return out;
}

void BatterySettings::load(int low, int warning, uint32_t revision) {
  state_.low_percent = low;
  state_.warning_percent = warning;
  state_.applied_revision = revision;
  state_.durable = true;
}

SettingsDecision BatterySettings::set(SettingsAuth auth, int schema, bool low_is_int, int low,
                                      bool warning_is_int, int warning, uint32_t base_revision,
                                      bool persist_ok) {
  if (auth == SettingsAuth::LanTokenUnsupported) {
    return decide(false, SettingsReason::LanTokenUnsupported, state_);
  }
  if (auth != SettingsAuth::Ok) return decide(false, SettingsReason::Unauthorized, state_);
  if (schema != kSchema) return decide(false, SettingsReason::UnsupportedSchema, state_);
  if (!low_is_int || !warning_is_int) return decide(false, SettingsReason::NotInteger, state_);
  if (warning <= kInhibit) return decide(false, SettingsReason::WarningNotAboveCutoff, state_);
  if (warning < kWarningMin || warning > kWarningMax || low < kLowMin || low > kLowMax) {
    return decide(false, SettingsReason::OutOfRange, state_);
  }
  if (low <= warning) return decide(false, SettingsReason::LowNotAboveWarning, state_);
  if (base_revision != state_.applied_revision) {
    return decide(false, SettingsReason::StaleRevision, state_);
  }
  if (!persist_ok) return decide(false, SettingsReason::PersistFailed, state_);
  state_.low_percent = low;
  state_.warning_percent = warning;
  state_.applied_revision += 1;
  state_.durable = true;
  return decide(true, SettingsReason::None, state_);
}

}  // namespace powercell
