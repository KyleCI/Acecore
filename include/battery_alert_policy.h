#pragma once

#include <stdint.h>

namespace powercell {

enum class BatteryAlertLevel { None, Low, Critical, ShutdownRisk };

struct BatteryReading {
  bool valid;
  float percent;
  float voltage;
  uint32_t receivedAtMs;
};

// All time comparisons use unsigned elapsed time, including across millis() wrap.
class BatteryAlertPolicy {
 public:
  static constexpr uint32_t FreshnessMs = 15000;
  static constexpr uint32_t StartupGraceMs = 15000;
  static constexpr float RecoveryMargin = 2.0f;
  enum Change : uint8_t { BatteryChanged = 1, AvailabilityChanged = 2 };

  explicit BatteryAlertPolicy(uint32_t startedAtMs = 0);
  uint8_t update(const BatteryReading& reading, uint32_t now);
  BatteryAlertLevel level() const { return level_; }
  bool available() const { return available_; }
  bool unavailableAlert() const { return unavailableAlert_; }
  static bool fresh(const BatteryReading& reading, uint32_t now);
  static uint32_t cooldown(BatteryAlertLevel level);

 private:
  BatteryAlertLevel nextLevel(float percent) const;
  BatteryAlertLevel level_ = BatteryAlertLevel::None;
  bool available_ = false;
  bool unavailableAlert_ = false;
  bool everAvailable_ = false;
  uint32_t startedAtMs_;
  uint32_t lastBatteryEventAt_ = 0;
};

}  // namespace powercell
