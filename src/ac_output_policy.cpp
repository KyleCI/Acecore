#include "ac_output_policy.h"

namespace powercell {

void AcOutputPolicy::setCeiling(int watts, AcMode mode) {
  ceiling_w_ = watts;
  mode_ = mode;
}

void AcOutputPolicy::setGates(bool rating_verified, bool polarity_verified) {
  rating_ = rating_verified;
  polarity_ = polarity_verified;
}

bool AcOutputPolicy::warnOperational() const { return ceiling_w_ > 0; }

bool AcOutputPolicy::shutdownOperational() const {
  return ceiling_w_ > 0 && rating_ && polarity_ && mode_ == AcMode::WarnAndShutdown;
}

bool AcOutputPolicy::takeShutdownEdge() {
  const bool edge = edge_;
  edge_ = false;
  return edge;
}

uint8_t AcOutputPolicy::update(const AcWatts& sample, uint32_t now) {
  const bool fresh = sample.valid && elapsed(now, sample.receivedAtMs) <= FreshnessMs;
  if (!fresh || ceiling_w_ <= 0) {
    unavailable_ = true;
    over_ = false;
    under_ = false;
    warning_ = false;
    return 0;
  }
  unavailable_ = false;
  if (!seen_) {
    seen_ = true;
    first_valid_ = now;
  }
  const bool surge = elapsed(now, first_valid_) < StartupGraceMs;
  const bool over = !surge && sample.watts > static_cast<float>(ceiling_w_);
  if (over) {
    if (!over_) {
      over_ = true;
      over_since_ = now;
    }
    under_ = false;
    if (warnOperational() && warn_armed_ && elapsed(now, over_since_) >= WarnHoldMs) {
      warning_ = true;
      warn_armed_ = false;
    }
    if (sample.coverage_verified && shutdownOperational() && !latched_ &&
        elapsed(now, over_since_) >= ShutdownHoldMs) {
      latched_ = true;
      edge_ = true;
    }
  } else {
    over_ = false;
    if (!under_) {
      under_ = true;
      under_since_ = now;
    }
    if (elapsed(now, under_since_) >= RearmHoldMs) {
      warning_ = false;
      warn_armed_ = true;
    }
    if (latched_ && elapsed(now, under_since_) >= ReleaseHoldMs) latched_ = false;
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
