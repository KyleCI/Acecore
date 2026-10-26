#pragma once
#include <stdint.h>
// Repeated-trip lockout (Acecore 2.2, Oct 22 2026). Pure logic, host tested from
// esp32_fw/test/host. Each relay trip (10 A software trip, or a user-limit trip)
// calls onTrip(). The 3rd trip within a rolling 5 min latches the lockout: no auto
// reclose, RELAY_ON refused, persisted by the caller in NVS. reset() (LoRa
// RESET_LOCKOUT or the socket button) clears the latch and the trip history.
class TripLockout {
 public:
  static constexpr uint8_t kTrips = 3;
  static constexpr uint32_t kWindowMs = 5UL * 60UL * 1000UL;
  // Returns true when this trip latched the lockout.
  bool onTrip(uint32_t nowMs, float amps) {
    lastAmps_ = amps;
    if (latched_) return false;
    times_[head_] = nowMs; head_ = (head_ + 1) % kTrips; if (count_ < kTrips) ++count_;
    if (count_ < kTrips) return false;
    uint32_t oldest = times_[head_];  // ring is full: head_ is the oldest of the last 3
    if (nowMs - oldest <= kWindowMs) { latched_ = true; return true; }
    return false;
  }
  void restore(bool latched, float amps) { latched_ = latched; lastAmps_ = amps; count_ = 0; head_ = 0; }
  void reset() { latched_ = false; count_ = 0; head_ = 0; }
  bool latched() const { return latched_; }
  float lastTripAmps() const { return lastAmps_; }
  uint8_t recentTrips(uint32_t nowMs) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < count_; ++i) n += nowMs - times_[i] <= kWindowMs;
    return n;
  }
 private:
  uint32_t times_[kTrips] = {};
  uint8_t head_ = 0, count_ = 0;
  bool latched_ = false;
  float lastAmps_ = 0;
};
