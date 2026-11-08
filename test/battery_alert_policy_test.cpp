#include "battery_alert_policy.h"
#include <assert.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>

using namespace powercell;

static BatteryReading reading(float percent, uint32_t now, bool charging = false) {
  return {true, percent, 51.2f, now, charging};
}

static void thresholds() {
  BatteryAlertPolicy p;
  assert(p.update(reading(50, 1), 1) == 0);
  assert(p.update(reading(25.1f, 2), 2) == 0);
  assert(p.update(reading(25, 3), 3) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(15.1f, 4), 4) == 0);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(15, 5), 5) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(!p.outputLatched());
  assert(p.update(reading(10.1f, 6), 6) == 0);
  assert(p.update(reading(10, 7), 7) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::ShutdownRisk);
  assert(p.outputLatched());
  assert(!p.outputOnAllowed());
  assert(p.takeOutputOffEdge());
  assert(!p.takeOutputOffEdge());
  assert(p.update(reading(0, 8), 8) == 0);
  assert(p.outputLatched());
}

static void fallingJump() {
  BatteryAlertPolicy p;
  assert(p.update(reading(50, 1), 1) == 0);
  assert(p.update(reading(8, 2), 2) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::ShutdownRisk);
  assert(p.outputLatched());
  assert(p.update(reading(8, 3), 3) == 0);
}

static void oneTimeWarningRearmsAfterHold() {
  BatteryAlertPolicy p;
  assert(p.update(reading(14, 1000), 1000) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(p.update(reading(14, 2000), 2000) == 0);
  assert(p.update(reading(15.1f, 3000), 3000) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(14, 4000), 4000) == 0);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(p.update(reading(20, 5000), 5000) & BatteryAlertPolicy::BatteryChanged);
  const uint32_t armed = 5000 + BatteryAlertPolicy::RecoveryHoldMs;
  assert(p.update(reading(20, armed), armed) == 0);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(14, armed + 1), armed + 1) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Critical);
}

static void latchReleasesOnlyAfterFifteenForOneMinute() {
  BatteryAlertPolicy p;
  p.update(reading(9, 1000), 1000);
  assert(p.outputLatched());
  assert(p.update(reading(14, 2000), 2000) == 0);
  assert(p.outputLatched());
  assert(p.update(reading(15, 3000, true), 3000) == 0);
  assert(p.outputLatched());
  assert(!p.outputOnAllowed());
  const uint32_t done = 3000 + BatteryAlertPolicy::RecoveryHoldMs;
  assert(p.update(reading(15, done - 1, true), done - 1) == 0);
  assert(p.outputLatched());
  assert(p.update(reading(16, done, true), done) & BatteryAlertPolicy::BatteryChanged);
  assert(!p.outputLatched());
  assert(p.outputOnAllowed());
  assert(p.level() == BatteryAlertLevel::Low);
  p.update(reading(9, done + 1), done + 1);
  assert(p.outputLatched());
  assert(!p.outputOnAllowed());
}

static void staleDoesNotLatchOrRelease() {
  BatteryAlertPolicy p;
  auto low = reading(8, 100);
  p.update(low, 100);
  assert(p.outputLatched());
  assert(p.update(low, 15100) == 0);
  assert(p.update(low, 15101) ==
      (BatteryAlertPolicy::BatteryChanged | BatteryAlertPolicy::AvailabilityChanged));
  assert(p.level() == BatteryAlertLevel::None);
  assert(p.outputLatched());
  assert(!p.outputOnAllowed());
  assert(p.update(reading(40, 20000, true), 20000) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::ShutdownRisk);
  assert(p.outputLatched());
  const uint32_t done = 20000 + BatteryAlertPolicy::RecoveryHoldMs;
  p.update(reading(40, done, true), done);
  assert(!p.outputLatched());
  assert(p.outputOnAllowed());
  assert(p.level() == BatteryAlertLevel::None);
}

static void persistence() {
  BatteryAlertPolicy first;
  first.update(reading(5, 10), 10);
  assert(first.latchDirty());
  assert(!first.latchDurable());
  first.ackLatchSave(false);
  assert(first.outputLatched());
  assert(!first.latchDurable());
  first.ackLatchSave(true);
  assert(first.latchDurable());
  assert(!first.latchDirty());

  BatteryAlertPolicy rebooted(50);
  rebooted.loadLatch(true, true);
  assert(rebooted.outputLatched());
  assert(!rebooted.outputOnAllowed());
  assert(rebooted.update(reading(12, 60), 60) == 0 || rebooted.outputLatched());
  assert(rebooted.outputLatched());

  BatteryAlertPolicy failed(50);
  failed.loadLatch(false, false);
  assert(!failed.outputOnAllowed());
  assert(!failed.outputLatched());
  failed.update(reading(80, 100, true), 100);
  assert(!failed.outputOnAllowed());
  failed.update(reading(80, 100 + BatteryAlertPolicy::RecoveryHoldMs, true),
                100 + BatteryAlertPolicy::RecoveryHoldMs);
  assert(failed.outputOnAllowed());
  assert(!failed.outputLatched());
}

static void hysteresis() {
  BatteryAlertPolicy p;
  assert(p.update(reading(25, 1), 1) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(27, 2), 2) == 0);
  assert(p.update(reading(27.1f, 3), 3) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::None);
  assert(p.update(reading(26, 4), 4) == 0);
  assert(p.update(reading(25, 5), 5) & BatteryAlertPolicy::BatteryChanged);
}

static void startupAndInvalidReadings() {
  BatteryAlertPolicy p(100);
  BatteryReading empty = {false, 0, 0, 0, false};
  assert(p.update(empty, 100) == 0);
  assert(p.update(empty, 15099) == 0);
  assert(p.update(empty, 15100) == BatteryAlertPolicy::AvailabilityChanged);
  assert(p.unavailableAlert());
  assert(p.level() == BatteryAlertLevel::None);
  assert(!p.outputLatched());
  assert(p.update(empty, 50000) == 0);
  assert(p.update(reading(50, 50001), 50001) == BatteryAlertPolicy::AvailabilityChanged);
  assert(!p.unavailableAlert());
  assert(p.outputOnAllowed());

  const float invalidPercent[] = {-1, 101, NAN, INFINITY};
  for (float percent : invalidPercent) {
    assert(!BatteryAlertPolicy::fresh(reading(percent, 1), 1));
  }
  auto r = reading(0, 1);
  r.voltage = 0;
  assert(!BatteryAlertPolicy::fresh(r, 1));
  r.voltage = NAN;
  assert(!BatteryAlertPolicy::fresh(r, 1));
  r = reading(0, 1);
  r.valid = false;
  assert(!BatteryAlertPolicy::fresh(r, 1));
}

static void cooldowns() {
  BatteryAlertPolicy low;
  const uint32_t start = 123;
  assert(low.update(reading(20, start), start) == BatteryAlertPolicy::BatteryChanged);
  const uint32_t deadline = start + BatteryAlertPolicy::cooldown(BatteryAlertLevel::Low);
  assert(low.update(reading(20, deadline - 1), deadline - 1) == 0);
  assert(low.update(reading(20, deadline), deadline) == BatteryAlertPolicy::BatteryChanged);

  BatteryAlertPolicy warning;
  assert(warning.update(reading(12, start), start) == BatteryAlertPolicy::BatteryChanged);
  assert(warning.update(reading(12, start + 60UL * 60 * 1000), start + 60UL * 60 * 1000) == 0);
  assert(warning.level() == BatteryAlertLevel::Critical);

  BatteryAlertPolicy inhibit;
  assert(inhibit.update(reading(5, start), start) == BatteryAlertPolicy::BatteryChanged);
  assert(inhibit.update(reading(5, start + 30UL * 60 * 1000), start + 30UL * 60 * 1000) == 0);
  assert(inhibit.outputLatched());
}

static void rollover() {
  const uint32_t start = UINT32_MAX - 100;
  BatteryAlertPolicy p(start);
  p.update(reading(20, start), start);
  assert(p.update(reading(20, 20), 20) == 0);
  const uint32_t later = start + BatteryAlertPolicy::cooldown(BatteryAlertLevel::Low);
  assert(p.update(reading(20, later), later) == BatteryAlertPolicy::BatteryChanged);
  assert(BatteryAlertPolicy::fresh(reading(20, start), 20));
  assert(!BatteryAlertPolicy::fresh(reading(20, start), 20000));

  BatteryAlertPolicy held(start);
  held.loadLatch(true, true);
  assert(held.outputLatched());
  assert(!held.outputOnAllowed());
  held.update(reading(20, start, true), start);
  const uint32_t release_at = start + BatteryAlertPolicy::RecoveryHoldMs;
  held.update(reading(20, release_at, true), release_at);
  assert(!held.outputLatched());
  assert(held.outputOnAllowed());
}

static void socWithoutChargingDoesNotRelease() {
  BatteryAlertPolicy p;
  p.update(reading(9, 1000), 1000);
  assert(p.outputLatched());
  const uint32_t done = 2000 + BatteryAlertPolicy::RecoveryHoldMs;
  assert(p.update(reading(40, 2000), 2000) == 0 || p.outputLatched());
  p.update(reading(40, done), done);
  assert(p.outputLatched());
  assert(!p.outputOnAllowed());
  p.update(reading(40, done + 1, true), done + 1);
  assert(p.outputLatched());
  p.update(reading(40, done + 1 + BatteryAlertPolicy::RecoveryHoldMs, true),
           done + 1 + BatteryAlertPolicy::RecoveryHoldMs);
  assert(!p.outputLatched());
}

static void configuredWarningDoesNotMoveHardCutoff() {
  BatteryAlertPolicy p;
  p.setThresholds(30, 20);
  assert(p.update(reading(19, 1), 1) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(!p.outputLatched());
  assert(p.update(reading(10, 2), 2) & BatteryAlertPolicy::BatteryChanged);
  assert(p.outputLatched());
  assert(p.update(reading(20, 3), 3) == 0);
  assert(p.outputLatched());
  p.update(reading(15, 4, true), 4);
  assert(p.outputLatched());
  const uint32_t done = 4 + BatteryAlertPolicy::RecoveryHoldMs;
  p.update(reading(15, done, true), done);
  assert(!p.outputLatched());
  assert(p.level() == BatteryAlertLevel::Critical);
}

int main() {
  thresholds();
  fallingJump();
  oneTimeWarningRearmsAfterHold();
  latchReleasesOnlyAfterFifteenForOneMinute();
  staleDoesNotLatchOrRelease();
  persistence();
  hysteresis();
  startupAndInvalidReadings();
  cooldowns();
  rollover();
  socWithoutChargingDoesNotRelease();
  configuredWarningDoesNotMoveHardCutoff();
  std::puts("PASS: battery thresholds, latch, warning, stale, persistence, rollover");
}
