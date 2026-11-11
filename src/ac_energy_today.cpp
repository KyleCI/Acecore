#include "ac_energy_today.h"

namespace powercell {

void AcEnergyToday::restore(bool have_record, int32_t day_id, uint32_t baseline_wh, bool day_unknown) {
  have_ = have_record && (isCalendarDay(day_id) || isSessionDay(day_id));
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

static bool clockUsable(const TodayInput& in) {
  if (in.clock == TodayClock::Session) return isSessionDay(in.day_id);
  return in.clock != TodayClock::Untrusted && isCalendarDay(in.day_id);
}

TodayOutput AcEnergyToday::observe(const TodayInput& in) {
  TodayOutput out;
  if (!in.valid) {
    known_ = false;
    return out;
  }
  if (!clockUsable(in)) {
    // No clock on this sample. Keep a baseline we already have, but do not
    // start a new one from an Untrusted input (Session is used for that).
    if (have_ && !unknown_) {
      known_ = true;
      out.known = true;
      out.kwh = kwh_;
    } else {
      known_ = false;
    }
    return out;
  }
  if (last_mono_ok_ && in.mono_continuous && static_cast<int32_t>(in.mono_ms - last_mono_) < 0) {
    known_ = false;
    return out;
  }

  const bool keep_calendar_during_session = have_ && isCalendarDay(day_) && isSessionDay(in.day_id);
  const bool adopt_session_to_calendar = have_ && isSessionDay(day_) && isCalendarDay(in.day_id);
  if (keep_calendar_during_session) {
    // Clock dropped after a real day was established. Keep that day.
  } else if (adopt_session_to_calendar) {
    day_ = in.day_id;
    out.persist = true;
  } else if (!have_ || day_ != in.day_id) {
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
  return out;
}

}  // namespace powercell
