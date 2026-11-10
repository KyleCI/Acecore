#pragma once

#include <stdint.h>

namespace powercell {

// Estimated AC energy for STM32 images. There is no AC watt-hour meter.
// Each fresh XFER4 / legacy status record supplies RMS volts and amps.
// Active watts are V * I * kPowerFactor (0.8). Watt-hours are that power
// times the monotonic interval. The DS3231 and the synced clock are UTC.
// Local midnight is UTC plus Africa/Lagos (WAT, +60 minutes, no DST), not
// the RTC's civil registers and not a phone-selected offset.
//
// dt comes from the monotonic millisecond clock. The wall clock is used for
// the Lagos day and for jump detection. A slice is not counted when the
// monotonic gap exceeds kMaxGapMs, monotonic time goes backwards, or the
// wall clock jumps by more than the monotonic interval plus kClockSlackS
// (the RTC resolves 1 second). An invalid or implausible V/I sample adds
// nothing; it is not treated as 0 W.
//
// The published lifetime is the last value saved to NVS, so a reboot cannot
// decrease a counter the server has already seen. Unsaved energy is at most
// one save interval at the image's rated power.

constexpr float kStmPowerFactor = 0.8f;
// 230 V inverter. 280 V is above a 5 kW / 3.5 kW output. 32 A is the 5 kW
// profile at 200 V and PF 0.8 (5000 / (200 * 0.8) = 31.25 A). The 3.5 kW
// profile is below that. A loaded output below 80 V is not a real reading.
constexpr float kStmMaxVolts = 280.f;
constexpr float kStmMaxAmps = 32.f;
constexpr float kStmMinLoadedVolts = 80.f;
constexpr float kStmIdleAmps = 0.2f;
// Status records are about 1 s apart (framed poll is 1 s; stale is 6 s).
// A slice longer than 10 s is a gap, not a rectangle of the previous power.
constexpr uint32_t kStmMaxGapMs = 10000;
constexpr int64_t kStmClockSlackS = 2;
// At 5000 W this bounds lost energy at 1250 Wh. At 3500 W, 875 Wh.
constexpr uint32_t kStmSaveMinMs = 15u * 60u * 1000u;
constexpr int kLagosOffsetMin = 60;

struct StmEstimateSample {
  bool valid = false;
  float volts = 0;
  float amps = 0;
  uint32_t mono_ms = 0;
  int64_t unix_s = 0;
  int32_t day_id = 0;
};

struct StmEstimateView {
  bool today_known = false;
  double today_kwh = 0;
  double lifetime_wh = 0;
  bool persist = false;
  int32_t persist_day_id = 0;
  double persist_today_wh = 0;
  double persist_lifetime_wh = 0;
};

class StmEnergyEstimate {
 public:
  static bool plausible(float volts, float amps);
  static float activeWatts(float volts, float amps);

  void restore(int32_t day_id, double today_wh, double lifetime_wh);
  StmEstimateView observe(const StmEstimateSample& sample);
  StmEstimateView publish() const { return makeView(); }
  void ackPersist(uint32_t mono_ms);

  double workingTodayWh() const { return today_wh_; }
  double workingLifetimeWh() const { return lifetime_wh_; }

 private:
  void noteDay(int32_t day_id);
  void reanchor(const StmEstimateSample& sample);
  StmEstimateView makeView() const;

  bool have_anchor_ = false;
  uint32_t last_mono_ = 0;
  int64_t last_unix_ = 0;
  uint32_t last_save_mono_ = 0;
  bool ever_saved_ = false;
  int32_t day_id_ = 0;
  int32_t clock_day_ = 0;
  double today_wh_ = 0;
  double lifetime_wh_ = 0;
  int32_t committed_day_ = 0;
  double committed_today_wh_ = 0;
  double committed_lifetime_wh_ = 0;
  bool dirty_ = false;
};

}  // namespace powercell

void stmEnergyBegin();
void stmEnergyService();
bool stmEnergyTodayKwh(double* kwh);
double stmEnergyLifetimeWh();
