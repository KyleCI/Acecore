#pragma once

#include <stdint.h>

namespace powercell {

enum class BatteryAlertLevel { None, Low, Critical, ShutdownRisk };

struct BatteryReading {
  bool valid;
  float percent;
  float voltage;
  uint32_t receivedAtMs;
  // True only when the same fresh JK-BMS frame reports charging (batStatus == 1).
  // Positive pack current is not used: this tree treats that as "not discharge",
  // which includes idle. Latch release requires this flag.
  bool charging;
};

// SOC is the JK-BMS byte in a fresh dynamic frame (0..100, positive pack
// voltage, age <= 15s). Stale or invalid SOC clears the visible level and
// does not start or release the discharge latch.
//
// Low:          enter <=25%, leave >27% (2 point margin), remind every 2h.
// Warning:      enter <=15%, one notification per episode.
// Output latch: enter <=10%. Release only after fresh SOC stays >=15% for
//               60s AND every sample in that window has charging==true.
//               SOC alone does not release. Release does not turn the output on.
// The warning rearms only after fresh SOC stays above the configured warning
// percent for 60s. That rearm does not require charging and does not release the latch.
class BatteryAlertPolicy {
 public:
  static constexpr uint32_t FreshnessMs = 15000;
  static constexpr uint32_t StartupGraceMs = 15000;
  static constexpr uint32_t RecoveryHoldMs = 60000;
  static constexpr float RecoveryMargin = 2.0f;
  static constexpr float LowPercent = 25.0f;
  static constexpr float WarningPercent = 15.0f;
  static constexpr float InhibitPercent = 10.0f;
  static constexpr float RestartPercent = 15.0f;
  enum Change : uint8_t { BatteryChanged = 1, AvailabilityChanged = 2 };

  explicit BatteryAlertPolicy(uint32_t startedAtMs = 0);
  uint8_t update(const BatteryReading& reading, uint32_t now);
  BatteryAlertLevel level() const { return level_; }
  bool available() const { return available_; }
  bool unavailableAlert() const { return unavailableAlert_; }
  bool outputLatched() const { return latched_; }
  bool outputOnAllowed() const { return !latched_ && !boot_unproven_; }
  bool latchDirty() const { return latch_dirty_; }
  bool latchDurable() const { return latch_durable_; }
  // Cold boot. A failed read blocks output-on until fresh recovery evidence.
  void loadLatch(bool read_ok, bool latched);
  void ackLatchSave(bool write_ok);
  bool takeOutputOffEdge();
  float lowPercent() const { return low_percent_; }
  float warningPercent() const { return warning_percent_; }
  // Caller has already validated ordering. Does not clear the latch or rearm a sent warning.
  void setThresholds(float low, float warning);
  static bool fresh(const BatteryReading& reading, uint32_t now);
  static uint32_t cooldown(BatteryAlertLevel level);

 private:
  static uint32_t elapsed(uint32_t now, uint32_t since) { return now - since; }
  BatteryAlertLevel levelFor(float percent) const;
  void considerLatch(float percent, bool charging, uint32_t now);
  void considerRearm(float percent, uint32_t now);

  BatteryAlertLevel level_ = BatteryAlertLevel::None;
  bool available_ = false;
  bool unavailableAlert_ = false;
  bool everAvailable_ = false;
  bool latched_ = false;
  bool latch_dirty_ = false;
  bool latch_durable_ = true;
  bool boot_unproven_ = false;
  bool warn_armed_ = true;
  bool latch_hold_ = false;
  bool warn_hold_ = false;
  bool output_off_edge_ = false;
  bool edge_is_new_ = false;
  uint32_t startedAtMs_;
  uint32_t lastBatteryEventAt_ = 0;
  uint32_t latch_hold_since_ = 0;
  uint32_t warn_hold_since_ = 0;
  float low_percent_ = LowPercent;
  float warning_percent_ = WarningPercent;
};

}  // namespace powercell
