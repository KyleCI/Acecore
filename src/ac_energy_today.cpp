#include "ac_energy_today.h"

namespace powercell {

void AcEnergyToday::restore(bool have_record, int32_t day_id, uint32_t baseline_wh, bool day_unknown) {
  have_ = have_record && day_id > 0;
  day_ = have_ ? day_id : 0;
  baseline_ = have_ ? baseline_wh : 0;
  unknown_ = have_ && day_unknown;
  last_wh_ = baseline_;
  last_mono_ = 0;
  last_mono_ok_ = false;
  known_ = false;
  kwh_ = 0;
}

bool AcEnergyToday::implausible(uint32_t step_wh, bool time_known, uint32_t elapsed_s) const {
  const uint64_t seconds = time_known ? (elapsed_s == 0 ? 1ull : static_cast<uint64_t>(elapsed_s))
                                      : static_cast<uint64_t>(kUnknownTimeCapS);
  const uint64_t cap = (static_cast<uint64_t>(kMaxPowerW) * seconds) / 3600ull + 2ull;
  return static_cast<uint64_t>(step_wh) > cap;
}

TodayOutput AcEnergyToday::observe(const TodayInput& in) {
  TodayOutput out;
  const bool clock_ok = in.clock != TodayClock::Untrusted && in.day_id > 0;
  if (!in.valid || !clock_ok) {
    known_ = false;
    return out;
  }
  if (last_mono_ok_ && in.mono_continuous && static_cast<int32_t>(in.mono_ms - last_mono_) < 0) {
    known_ = false;
    return out;
  }

  if (!have_ || day_ != in.day_id) {
    have_ = true;
    unknown_ = false;
    day_ = in.day_id;
    baseline_ = in.lifetime_wh;
    last_wh_ = in.lifetime_wh;
    last_mono_ = in.mono_ms;
    last_mono_ok_ = in.mono_continuous;
    known_ = true;
    kwh_ = 0;
    out.known = true;
    out.kwh = 0;
    out.persist = true;
    return out;
  }

  if (unknown_) {
    known_ = false;
    return out;
  }

  if (in.lifetime_wh < last_wh_) {
    baseline_ = in.lifetime_wh;
    last_wh_ = in.lifetime_wh;
    last_mono_ = in.mono_ms;
    last_mono_ok_ = in.mono_continuous;
    unknown_ = true;
    known_ = false;
    kwh_ = 0;
    out.persist = true;
    return out;
  }

  const bool time_known = in.mono_continuous && last_mono_ok_;
  const uint32_t elapsed_s = time_known ? (in.mono_ms - last_mono_) / 1000u : 0;
  const uint32_t step = in.lifetime_wh - last_wh_;
  if (implausible(step, time_known, elapsed_s)) {
    baseline_ = in.lifetime_wh;
    last_wh_ = in.lifetime_wh;
    last_mono_ = in.mono_ms;
    last_mono_ok_ = in.mono_continuous;
    unknown_ = true;
    known_ = false;
    kwh_ = 0;
    out.persist = true;
    return out;
  }

  last_wh_ = in.lifetime_wh;
  last_mono_ = in.mono_ms;
  last_mono_ok_ = in.mono_continuous;
  known_ = true;
  kwh_ = static_cast<double>(in.lifetime_wh - baseline_) / 1000.0;
  out.known = true;
  out.kwh = kwh_;
  out.persist = false;
  return out;
}

}  // namespace powercell
