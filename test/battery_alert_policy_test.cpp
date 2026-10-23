#include "battery_alert_policy.h"
#include <assert.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>

using namespace powercell;

static BatteryReading reading(float percent, uint32_t now) {
  return {true, percent, 51.2f, now};
}

static void thresholds() {
  BatteryAlertPolicy p;
  assert(p.update(reading(50, 1), 1) == 0);
  assert(p.update(reading(25.1f, 2), 2) == 0);
  assert(p.update(reading(25, 3), 3) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(10, 4), 4) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(p.update(reading(5, 5), 5) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::ShutdownRisk);
  assert(p.update(reading(0, 6), 6) == 0);  // Genuine fresh 0% stays critical.
}

static void hysteresis() {
  BatteryAlertPolicy p;
  p.update(reading(5, 1), 1);
  assert(p.update(reading(7, 2), 2) == 0);
  p.update(reading(7.1f, 3), 3);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(p.update(reading(12, 4), 4) == 0);
  p.update(reading(12.1f, 5), 5);
  assert(p.level() == BatteryAlertLevel::Low);
  assert(p.update(reading(27, 6), 6) == 0);
  assert(p.update(reading(27.1f, 7), 7) & BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::None);
  assert(p.update(reading(26, 8), 8) == 0);
  assert(p.update(reading(25, 9), 9) & BatteryAlertPolicy::BatteryChanged);
}

static void startupAndInvalidReadings() {
  BatteryAlertPolicy p(100);
  BatteryReading empty = {false, 0, 0, 0};
  assert(p.update(empty, 100) == 0);
  assert(p.update(empty, 15099) == 0);
  assert(p.update(empty, 15100) == BatteryAlertPolicy::AvailabilityChanged);
  assert(p.unavailableAlert());
  assert(p.level() == BatteryAlertLevel::None);
  assert(p.update(empty, 50000) == 0);
  assert(p.update(reading(50, 50001), 50001) == BatteryAlertPolicy::AvailabilityChanged);
  assert(!p.unavailableAlert());

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

static void staleAndReconnect() {
  BatteryAlertPolicy p;
  auto r = reading(8, 100);
  p.update(r, 100);
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(p.update(r, 15100) == 0);
  assert(p.update(r, 15101) ==
      (BatteryAlertPolicy::BatteryChanged | BatteryAlertPolicy::AvailabilityChanged));
  assert(p.level() == BatteryAlertLevel::None);
  assert(p.unavailableAlert());
  assert(p.update(r, 100000) == 0);
  assert(p.update(reading(8, 100001), 100001) ==
      (BatteryAlertPolicy::BatteryChanged | BatteryAlertPolicy::AvailabilityChanged));
  assert(p.level() == BatteryAlertLevel::Critical);
  assert(p.update(reading(50, 100002), 100002) == BatteryAlertPolicy::BatteryChanged);
  assert(p.level() == BatteryAlertLevel::None);
}

static void cooldowns() {
  for (auto level : {BatteryAlertLevel::Low, BatteryAlertLevel::Critical, BatteryAlertLevel::ShutdownRisk}) {
    const float percent = level == BatteryAlertLevel::Low ? 20 : level == BatteryAlertLevel::Critical ? 8 : 3;
    BatteryAlertPolicy p;
    const uint32_t start = 123;
    assert(p.update(reading(percent, start), start) == BatteryAlertPolicy::BatteryChanged);
    const uint32_t deadline = start + BatteryAlertPolicy::cooldown(level);
    assert(p.update(reading(percent, deadline - 1), deadline - 1) == 0);
    assert(p.update(reading(percent, deadline), deadline) == BatteryAlertPolicy::BatteryChanged);
    assert(p.update(reading(percent, deadline + 1), deadline + 1) == 0);
  }
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
}

int main() {
  thresholds();
  hysteresis();
  startupAndInvalidReadings();
  staleAndReconnect();
  cooldowns();
  rollover();
  std::puts("PASS: thresholds, hysteresis, invalid/startup readings, stale/reconnect, cooldowns, millis rollover");
}
