#pragma once

#include <stdint.h>

namespace powercell {

// Today's consumed AC output energy from a PZEM-004T lifetime watt-hour counter.
//
// The number is (counter - baseline) since the baseline for this local day.
// The baseline is the counter at the first trusted sample of the day, or the
// baseline saved earlier that same day (so a reboot keeps energy counted while
// the ESP was off — the meter keeps running). It is not integrated from watts.
//
// Local day uses the configured timezone offset. A calendar midnight rolls
// only when the clock is synced: NTP, phone time, or a coherent DS3231.
// With no trusted clock (offline, no NTP/phone, RTC untrusted) the first
// valid PZEM sample still starts a session baseline, and today is
// (counter - that baseline). A reboot keeps that session baseline. When a
// trusted clock later arrives, the session is adopted as the real local day
// and the energy already counted stays in today. Midnight is not invented
// while the clock is unsynced.
//
// A missing or invalid meter sample publishes nothing (null), not zero.
// Zero is published only when a real baseline and a real counter are equal.
// A backwards counter or an implausible jump (reset or rollover) makes the
// rest of that local day unknown. The next trusted day starts a new baseline.
//
// Persistence is the baseline, the day, and the unknown flag. The counter
// itself is not copied on every sample.

enum class TodayClock : uint8_t { Untrusted = 0, Rtc = 1, Anchored = 2, Session = 3 };

// Not a calendar day. Used while the clock is unsynced so today can still
// be published from the first valid PZEM reading.
constexpr int32_t kSessionDayId = -1;

inline bool isCalendarDay(int32_t day_id) { return day_id > 0; }
inline bool isSessionDay(int32_t day_id) { return day_id == kSessionDayId; }

struct TodayInput {
  bool valid = false;
  uint32_t lifetime_wh = 0;
  int32_t day_id = 0;
  TodayClock clock = TodayClock::Untrusted;
  uint32_t mono_ms = 0;
  bool mono_continuous = false;
};

struct TodayOutput {
  bool known = false;
  double kwh = 0;
  bool persist = false;
};

class AcEnergyToday {
 public:
  static constexpr uint32_t kMaxPowerW = 26000;
  static constexpr uint32_t kUnknownTimeCapS = 48u * 3600u;

  void restore(bool have_record, int32_t day_id, uint32_t baseline_wh, bool day_unknown);
  TodayOutput observe(const TodayInput& in);

  bool known() const { return known_; }
  double kwh() const { return kwh_; }
  int32_t dayId() const { return day_; }
  uint32_t baselineWh() const { return baseline_; }
  bool baselineValid() const { return have_; }
  bool dayUnknown() const { return unknown_; }

 private:
  bool implausible(uint32_t step_wh, bool time_known, uint32_t elapsed_s) const;

  bool have_ = false;
  bool unknown_ = false;
  bool known_ = false;
  int32_t day_ = 0;
  uint32_t baseline_ = 0;
  uint32_t last_wh_ = 0;
  uint32_t last_mono_ = 0;
  bool last_mono_ok_ = false;
  double kwh_ = 0;
};

}  // namespace powercell
