#pragma once
#include <stdint.h>
// 2.5.8: PV energy integrated from the ADS1115 power readings.
//  - today: resets when the local day (epoch + phone tz) changes; the day is only
//    known with a trusted clock (NTP or phone POST /v2/time). Without one, today
//    keeps counting on the last known day.
//  - total: lifetime, never resets except FACTORY_WIPE of the namespace.
//  - integration: trapezoid-free rectangle P x dt on valid samples; a gap over
//    kMaxGapMs (I2C back-off, invalid reads) is not integrated. Negative P
//    (shunt offset at night) counts as 0.
//  - NVS: saveDue() when unsaved energy >= kSaveWh and at least kSaveMinMs have
//    passed since the last save, or when unsaved energy > kSaveIdleWh has waited
//    kSaveIdleMs, or on a day change. NVS keys stay tot_wh, tdy_wh, day in
//    namespace "solar", so existing totals still load. This caps a power-cut
//    loss at about 5 min of production (~250 Wh at 3 kW). The 15 min idle path
//    flushes a trickle above 0.5 Wh. Worst case is a write every 5 min, or 288
//    writes/day of those 3 small keys, which is fine for ESP32 NVS wear
//    levelling (the partition spreads erase cycles well beyond a 100k raw
//    endurance budget at this rate).
namespace pc {
namespace solar {
constexpr uint32_t kMaxGapMs = 10000;
constexpr double kSaveWh = 10.0;
constexpr uint32_t kSaveMinMs = 5UL * 60UL * 1000UL;
constexpr double kSaveIdleWh = 0.5;
constexpr uint32_t kSaveIdleMs = 15UL * 60UL * 1000UL;
constexpr int32_t kNoDay = -1;

// Local day number (days since 1970 in local time) or kNoDay when epoch untrusted.
int32_t localDay(int64_t epoch, bool trusted, int tzOffsetMin);

class Energy {
 public:
  void restore(double totalWh, double todayWh, int32_t day);
  void update(bool valid, float powerW, uint32_t nowMs, int32_t day);
  bool saveDue(uint32_t nowMs) const;
  void markSaved(uint32_t nowMs);
  double totalWh() const { return total_; }
  double todayWh() const { return today_; }
  int32_t day() const { return day_; }

 private:
  double total_ = 0, today_ = 0, savedTotal_ = 0;
  int32_t day_ = kNoDay;
  bool havePrev_ = false, dayChanged_ = false, everSaved_ = false;
  uint32_t prevMs_ = 0, savedMs_ = 0;
};
}  // namespace solar
}  // namespace pc
