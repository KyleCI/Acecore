#pragma once

#include <stdint.h>

namespace powercell {

// Manual ceiling compares against the v0.1.1 PZEM-004T active-power register
// (power_dW, 0.1 W per count; telemetry ac_consumed.active_power_w).
// coverage_verified is a separate fact: the meter is not confirmed to be the
// whole inverter output, and it is not calibrated here. BMS, solar, VA, and
// plug sums are not inputs. A missing or stale sample is unavailable, not 0 W,
// and it does not start or clear a shutdown.
enum class AcMode : uint8_t { WarnOnly = 0, WarnAndShutdown = 1 };

struct AcWatts {
  bool valid = false;
  bool coverage_verified = false;
  float watts = 0;
  uint32_t receivedAtMs = 0;
};

class AcOutputPolicy {
 public:
  static constexpr uint32_t FreshnessMs = 2000;
  static constexpr uint32_t StartupGraceMs = 3000;
  static constexpr uint32_t WarnHoldMs = 3000;
  static constexpr uint32_t ShutdownHoldMs = 8000;
  static constexpr uint32_t RearmHoldMs = 10000;
  static constexpr uint32_t ReleaseHoldMs = 30000;

  void setUsable(int usable_w);
  void setCeiling(int watts, AcMode mode);
  uint8_t update(const AcWatts& sample, uint32_t now);
  bool coverageUnavailable() const { return unavailable_; }
  const char* unavailableReason() const { return reason_; }
  bool warning() const { return warning_; }
  bool shutdownLatched() const { return latched_; }
  bool systemOverloadLatched() const { return system_latched_; }
  bool takeShutdownEdge();
  bool turnsOutputOn() const { return false; }
  int usableW() const { return usable_w_; }
  // This object does not write a pin. physical_shutdown_verified stays false.
  static constexpr bool kDrivesOutput = false;
  static constexpr bool kCoverageVerified = false;

 private:
  static uint32_t elapsed(uint32_t now, uint32_t since) { return now - since; }
  int usable_w_ = 0;
  int ceiling_w_ = 0;
  AcMode mode_ = AcMode::WarnOnly;
  const char* reason_ = "usable_unset";
  bool unavailable_ = true;
  bool warning_ = false;
  bool warn_armed_ = true;
  bool latched_ = false;
  bool system_latched_ = false;
  bool edge_ = false;
  bool system_over_ = false;
  bool ceiling_over_ = false;
  bool under_ = false;
  bool seen_ = false;
  uint32_t system_since_ = 0;
  uint32_t ceiling_since_ = 0;
  uint32_t under_since_ = 0;
  uint32_t first_valid_ = 0;
};

void acOutputObserve(const AcWatts& sample, uint32_t now);
AcOutputPolicy& acOutputPolicy();

}  // namespace powercell
