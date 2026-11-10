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
    case BatteryAlertLevel::Critical: return 0;
    case BatteryAlertLevel::ShutdownRisk: return 0;
    default: return 0;
  }
}

BatteryAlertLevel BatteryAlertPolicy::levelFor(float percent) const {
  if (latched_ || percent <= InhibitPercent) return BatteryAlertLevel::ShutdownRisk;
  if (percent <= warning_percent_) return BatteryAlertLevel::Critical;
  if (percent <= low_percent_ ||
      (level_ == BatteryAlertLevel::Low && percent <= low_percent_ + RecoveryMargin)) {
    return BatteryAlertLevel::Low;
  }
  return BatteryAlertLevel::None;
}

void BatteryAlertPolicy::loadLatch(bool read_ok, bool latched) {
  if (!read_ok) {
    boot_unproven_ = true;
    latch_durable_ = false;
    output_off_edge_ = true;
    return;
  }
  latched_ = latched;
  boot_unproven_ = false;
  latch_durable_ = true;
  latch_dirty_ = false;
  if (latched_) {
    warn_armed_ = false;
    output_off_edge_ = true;
  }
}

void BatteryAlertPolicy::ackLatchSave(bool write_ok) {
  latch_durable_ = write_ok;
  if (write_ok) latch_dirty_ = false;
}

bool BatteryAlertPolicy::takeOutputOffEdge() {
  const bool edge = output_off_edge_;
  output_off_edge_ = false;
  return edge;
}

void BatteryAlertPolicy::considerLatch(float percent, bool bms_own, uint32_t now) {
  if (percent <= InhibitPercent) {
    latch_hold_ = false;
    if (!latched_) {
      latched_ = true;
      latch_dirty_ = true;
      latch_durable_ = false;
      output_off_edge_ = true;
      edge_is_new_ = true;
      boot_unproven_ = false;
      warn_armed_ = false;
    }
    return;
  }
  if ((latched_ || boot_unproven_) && bms_own && percent >= RestartPercent) {
    if (!latch_hold_) {
      latch_hold_ = true;
      latch_hold_since_ = now;
    } else if (elapsed(now, latch_hold_since_) >= RecoveryHoldMs) {
      // Clearing either a saved latch or an unreadable-boot hold must be
      // written. A RAM-only clear comes back as battery_critical_latched
      // on the next boot, which is when the websocket reconnects.
      const bool blocking = latched_ || boot_unproven_;
      if (latched_) latched_ = false;
      boot_unproven_ = false;
      latch_hold_ = false;
      if (blocking) {
        latch_dirty_ = true;
        latch_durable_ = false;
      }
    }
  } else {
    latch_hold_ = false;
  }
}

void BatteryAlertPolicy::setThresholds(float low, float warning) {
  low_percent_ = low;
  warning_percent_ = warning;
}

void BatteryAlertPolicy::considerRearm(float percent, uint32_t now) {
  if (warn_armed_ || percent <= warning_percent_) {
    warn_hold_ = false;
    return;
  }
  if (!warn_hold_) {
    warn_hold_ = true;
    warn_hold_since_ = now;
  } else if (elapsed(now, warn_hold_since_) >= RecoveryHoldMs) {
    warn_armed_ = true;
    warn_hold_ = false;
  }
}

uint8_t BatteryAlertPolicy::update(const BatteryReading& reading, uint32_t now) {
  uint8_t changes = 0;
  available_ = fresh(reading, now);
  if (available_) everAvailable_ = true;
  const bool unavailable = !available_ &&
      (everAvailable_ || elapsed(now, startedAtMs_) >= StartupGraceMs);
  if (unavailable != unavailableAlert_) {
    unavailableAlert_ = unavailable;
    changes |= AvailabilityChanged;
  }

  if (!available_) {
    latch_hold_ = false;
    warn_hold_ = false;
  } else {
    considerLatch(reading.percent, reading.bms_own, now);
    considerRearm(reading.percent, now);
  }

  const BatteryAlertLevel next = available_ ? levelFor(reading.percent) : BatteryAlertLevel::None;
  bool notify = false;
  if (next != level_) {
    if (next == BatteryAlertLevel::Critical || next == BatteryAlertLevel::ShutdownRisk) {
      if (warn_armed_ || next == BatteryAlertLevel::ShutdownRisk) {
        notify = true;
        warn_armed_ = false;
      }
    } else {
      notify = true;
    }
    level_ = next;
  } else if (next == BatteryAlertLevel::Low &&
             elapsed(now, lastBatteryEventAt_) >= cooldown(BatteryAlertLevel::Low)) {
    notify = true;
  }
  if (edge_is_new_) {
    notify = true;
    edge_is_new_ = false;
  }
  if (notify) {
    lastBatteryEventAt_ = now;
    changes |= BatteryChanged;
  }
  return changes;
}

}  // namespace powercell
