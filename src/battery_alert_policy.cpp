#include "battery_alert_policy.h"
#include <math.h>

namespace powercell {

BatteryAlertPolicy::BatteryAlertPolicy(uint32_t startedAtMs)
    : startedAtMs_(startedAtMs) {}

bool BatteryAlertPolicy::fresh(const BatteryReading& reading, uint32_t now) {
  return reading.valid && isfinite(reading.percent) &&
      reading.percent >= 0.0f && reading.percent <= 100.0f &&
      isfinite(reading.voltage) && reading.voltage > 0.0f &&
      static_cast<uint32_t>(now - reading.receivedAtMs) <= FreshnessMs;
}

uint32_t BatteryAlertPolicy::cooldown(BatteryAlertLevel level) {
  switch (level) {
    case BatteryAlertLevel::Low: return 2UL * 60 * 60 * 1000;
    case BatteryAlertLevel::Critical: return 60UL * 60 * 1000;
    case BatteryAlertLevel::ShutdownRisk: return 30UL * 60 * 1000;
    default: return 0;
  }
}

BatteryAlertLevel BatteryAlertPolicy::nextLevel(float percent) const {
  if (percent <= 5.0f ||
      (level_ == BatteryAlertLevel::ShutdownRisk && percent <= 5.0f + RecoveryMargin)) {
    return BatteryAlertLevel::ShutdownRisk;
  }
  if (percent <= 10.0f ||
      (level_ == BatteryAlertLevel::Critical && percent <= 10.0f + RecoveryMargin)) {
    return BatteryAlertLevel::Critical;
  }
  // if (percent <= 25.0f ||
  //     (level_ == BatteryAlertLevel::Low && percent <= 25.0f + RecoveryMargin)) {
  //   return BatteryAlertLevel::Low;
  // }
  if (percent <= 89.0f ||
      (level_ == BatteryAlertLevel::Low && percent <= 89.0f + RecoveryMargin)) {
    return BatteryAlertLevel::Low;
  }
  return BatteryAlertLevel::None;
}

uint8_t BatteryAlertPolicy::update(const BatteryReading& reading, uint32_t now) {
  uint8_t changes = 0;
  available_ = fresh(reading, now);
  if (available_) everAvailable_ = true;
  const bool unavailable = !available_ &&
      (everAvailable_ || static_cast<uint32_t>(now - startedAtMs_) >= StartupGraceMs);
  if (unavailable != unavailableAlert_) {
    unavailableAlert_ = unavailable;
    changes |= AvailabilityChanged;
  }

  const BatteryAlertLevel next = available_ ? nextLevel(reading.percent) : BatteryAlertLevel::None;
  const bool repeat = next != BatteryAlertLevel::None &&
      static_cast<uint32_t>(now - lastBatteryEventAt_) >= cooldown(next);
  if (next != level_ || repeat) {
    level_ = next;
    lastBatteryEventAt_ = now;
    changes |= BatteryChanged;
  }
  return changes;
}

}  // namespace powercell
