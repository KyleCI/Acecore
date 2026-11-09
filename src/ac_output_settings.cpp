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
                                 bool mode_known, int mode, int usable_w, uint32_t base_revision,
                                 bool persist_ok) {
  AcDecision out = readback();
  if (auth != SettingsAuth::Ok) {
    out.reason = auth == SettingsAuth::SessionOnly ? SettingsReason::SessionNotSufficient
                 : auth == SettingsAuth::LanTokenUnsupported ? SettingsReason::LanTokenUnsupported
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
  if (usable_w <= 0) {
    out.reason = SettingsReason::UsableUnset;
    return out;
  }
  if (ceiling_w > usable_w) {
    out.reason = SettingsReason::CeilingAboveUsable;
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

int SystemRating::usableFor(int rated_active_w) {
  if (rated_active_w == kRated5kW) return kUsable5kW;
  if (rated_active_w == kRated35kW) return kUsable35kW;
  return 0;
}

SystemRating::SystemRating(int factory_rated_w, const char* variant)
    : factory_rated_w_(factory_rated_w), variant_(variant && variant[0] ? variant : "unrated") {
  evaluate();
}

void SystemRating::observeStore(bool present, int stored_rated_w) {
  store_present_ = present;
  stored_rated_w_ = stored_rated_w;
  evaluate();
}

const char* SystemRating::reason() const {
  switch (state_.availability) {
    case RatingAvailability::Armed:
      return "none";
    case RatingAvailability::Unprovisioned:
      return "rating_unprovisioned";
    case RatingAvailability::Unknown:
      return "unknown_rating";
    case RatingAvailability::Mismatch:
      return "rating_mismatch";
    case RatingAvailability::InvalidStore:
      return "rating_invalid";
  }
  return "rating_unprovisioned";
}

void SystemRating::evaluate() {
  state_.schema = 1;
  state_.factory_rated_w = factory_rated_w_;
  state_.variant = variant_;
  state_.store_present = store_present_;
  state_.stored_rated_w = store_present_ ? stored_rated_w_ : 0;
  state_.physical_shutdown_verified = false;
  const int derived = usableFor(factory_rated_w_);
  if (factory_rated_w_ == 0) {
    state_.availability = RatingAvailability::Unprovisioned;
  } else if (derived == 0) {
    state_.availability = RatingAvailability::Unknown;
  } else if (store_present_ && stored_rated_w_ != factory_rated_w_) {
    state_.availability = usableFor(stored_rated_w_) == 0 ? RatingAvailability::InvalidStore
                                                          : RatingAvailability::Mismatch;
  } else {
    state_.availability = RatingAvailability::Armed;
  }
  state_.overload_configured = state_.availability == RatingAvailability::Armed;
  state_.usable_active_w = state_.overload_configured ? derived : 0;
}

}  // namespace powercell
