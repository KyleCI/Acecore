#include "ac_output_policy.h"

namespace powercell {
namespace {
void arm(bool over, bool* holding, uint32_t* since, uint32_t now) {
  if (!over) {
    *holding = false;
    return;
  }
  if (!*holding) {
    *holding = true;
    *since = now;
  }
}
}  // namespace

void AcOutputPolicy::setUsable(int usable_w) { usable_w_ = usable_w > 0 ? usable_w : 0; }

void AcOutputPolicy::setCeiling(int watts, AcMode mode) {
  ceiling_w_ = watts > 0 ? watts : 0;
  mode_ = mode;
}

bool AcOutputPolicy::takeShutdownEdge() {
  const bool edge = edge_;
  edge_ = false;
  return edge;
}

uint8_t AcOutputPolicy::update(const AcWatts& sample, uint32_t now) {
  const bool fresh = sample.valid && elapsed(now, sample.receivedAtMs) <= FreshnessMs;
  if (!fresh) {
    unavailable_ = true;
    reason_ = "stale_or_invalid";
    system_over_ = false;
    ceiling_over_ = false;
    under_ = false;
    warning_ = false;
    return 0;
  }
  if (usable_w_ <= 0) {
    unavailable_ = true;
    reason_ = "usable_unset";
    system_over_ = false;
    ceiling_over_ = false;
    under_ = false;
    warning_ = false;
    return 0;
  }
  unavailable_ = false;
  reason_ = kCoverageVerified ? "ok" : "placement_unverified";
  if (!seen_) {
    seen_ = true;
    first_valid_ = now;
  }
  const bool surge = elapsed(now, first_valid_) < StartupGraceMs;
  const bool system_over = !surge && sample.watts > static_cast<float>(usable_w_);
  const bool ceiling_over = !surge && ceiling_w_ > 0 && sample.watts > static_cast<float>(ceiling_w_);
  const bool ceiling_shutdown = ceiling_over && mode_ == AcMode::WarnAndShutdown;
  arm(system_over, &system_over_, &system_since_, now);
  arm(ceiling_over, &ceiling_over_, &ceiling_since_, now);
  if (system_over || ceiling_over) {
    under_ = false;
    if (warn_armed_ &&
        ((system_over_ && elapsed(now, system_since_) >= WarnHoldMs) ||
         (ceiling_over_ && elapsed(now, ceiling_since_) >= WarnHoldMs))) {
      warning_ = true;
      warn_armed_ = false;
    }
    const bool system_due = system_over_ && elapsed(now, system_since_) >= ShutdownHoldMs;
    const bool ceiling_due = ceiling_shutdown && elapsed(now, ceiling_since_) >= ShutdownHoldMs;
    if ((system_due || ceiling_due) && !latched_) {
      latched_ = true;
      system_latched_ = system_due;
      edge_ = true;
    } else if (system_due) {
      system_latched_ = true;
    }
  } else {
    system_over_ = false;
    ceiling_over_ = false;
    if (!under_) {
      under_ = true;
      under_since_ = now;
    }
    if (elapsed(now, under_since_) >= RearmHoldMs) {
      warning_ = false;
      warn_armed_ = true;
    }
    if (latched_ && elapsed(now, under_since_) >= ReleaseHoldMs) {
      latched_ = false;
      system_latched_ = false;
    }
  }
  return 0;
}

AcOutputPolicy& acOutputPolicy() {
  static AcOutputPolicy policy;
  return policy;
}

void acOutputObserve(const AcWatts& sample, uint32_t now) {
  acOutputPolicy().update(sample, now);
}

}  // namespace powercell
