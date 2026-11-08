#include "ac_output_settings.h"

namespace powercell {

AcDecision AcOutputSettings::readback() const {
  AcDecision out;
  out.readback = state_;
  out.readback.warn_operational = false;
  out.readback.shutdown_operational = false;
  out.readback.physical_shutdown_verified = false;
  return out;
}

void AcOutputSettings::load(int ceiling_w, int mode, uint32_t revision) {
  state_.ceiling_w = ceiling_w;
  state_.mode = mode == 1 ? AcModeSetting::WarnAndShutdown : AcModeSetting::WarnOnly;
  state_.applied_revision = revision;
  state_.durable = true;
}

AcDecision AcOutputSettings::set(SettingsAuth auth, int schema, bool ceiling_is_int, int ceiling_w,
                                 bool mode_known, int mode, uint32_t base_revision, bool persist_ok) {
  AcDecision out = readback();
  if (auth != SettingsAuth::Ok) {
    out.reason = auth == SettingsAuth::LanTokenUnsupported ? SettingsReason::LanTokenUnsupported
                                                           : SettingsReason::Unauthorized;
    return out;
  }
  if (schema != kSchema) {
    out.reason = SettingsReason::UnsupportedSchema;
    return out;
  }
  if (!ceiling_is_int || !mode_known) {
    out.reason = SettingsReason::NotInteger;
    return out;
  }
  if (mode != 0 && mode != 1) {
    out.reason = SettingsReason::OutOfRange;
    return out;
  }
  if (ceiling_w < kMinW || ceiling_w > kMaxW) {
    out.reason = SettingsReason::OutOfRange;
    return out;
  }
  if (base_revision != state_.applied_revision) {
    out.reason = SettingsReason::StaleRevision;
    return out;
  }
  if (!persist_ok) {
    out.reason = SettingsReason::PersistFailed;
    return out;
  }
  state_.ceiling_w = ceiling_w;
  state_.mode = mode == 1 ? AcModeSetting::WarnAndShutdown : AcModeSetting::WarnOnly;
  state_.applied_revision += 1;
  state_.durable = true;
  out = readback();
  out.applied = true;
  out.durable_commit = true;
  out.reason = SettingsReason::None;
  return out;
}

}  // namespace powercell
