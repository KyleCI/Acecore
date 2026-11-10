#include "solar_energy.h"
#include <math.h>

namespace pc {
namespace solar {

int32_t localDay(int64_t epoch, bool trusted, int tzOffsetMin) {
  if (!trusted || epoch < 1700000000LL) return kNoDay;
  int64_t l = epoch + int64_t(tzOffsetMin) * 60;
  return int32_t(l / 86400);
}

void Energy::restore(double totalWh, double todayWh, int32_t day) {
  total_ = (isfinite(totalWh) && totalWh > 0) ? totalWh : 0;
  today_ = (isfinite(todayWh) && todayWh > 0) ? todayWh : 0;
  if (today_ > total_) total_ = today_;
  day_ = day; savedTotal_ = total_;
}

void Energy::update(bool valid, float powerW, uint32_t nowMs, int32_t day) {
  if (day != kNoDay && day != day_) {
    // Unknown -> known: adopt the day (energy counted before the clock arrived stays in today).
    // Known -> another day: new local day, today starts at 0 and the rollover is saved.
    if (day_ != kNoDay) { today_ = 0; dayChanged_ = true; }
    day_ = day;
  }
  if (!valid || !isfinite(powerW)) { havePrev_ = false; return; }
  if (havePrev_) {
    uint32_t dt = nowMs - prevMs_;
    if (dt <= kMaxGapMs && powerW > 0) {
      double wh = double(powerW) * double(dt) / 3600000.0;
      total_ += wh; today_ += wh;
    }
  }
  havePrev_ = true; prevMs_ = nowMs;
}

bool Energy::saveDue(uint32_t nowMs) const {
  if (dayChanged_) return true;
  if (total_ - savedTotal_ < kSaveWh) return false;
  return !everSaved_ || nowMs - savedMs_ >= kSaveMinMs;
}

void Energy::markSaved(uint32_t nowMs) {
  savedTotal_ = total_; savedMs_ = nowMs; everSaved_ = true; dayChanged_ = false;
}
}  // namespace solar
}  // namespace pc
