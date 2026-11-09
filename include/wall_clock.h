#pragma once

#include "time_policy.h"

#include <stdint.h>

namespace powercell {

// Same decision as deployed 2.5.11 clock_::setFromPhone, plus the v0.1.1 DS3231 write.
// Hooks are the device settimeofday / RTC / NVS path. Host tests install the same hooks.
struct ClockHooks {
  bool (*set_system)(int64_t epoch, void* ctx);
  bool (*write_rtc)(int64_t epoch, void* ctx);
  bool (*persist)(int64_t epoch, bool have_epoch, int tz, void* ctx);
  void* ctx;
};

struct ClockEffect {
  bool applied = false;
  bool system_set = false;
  const char* rtc = "not_written";
  bool nvs_persisted = false;
  pc::clk::Verdict verdict = pc::clk::Verdict::Invalid;
  int64_t epoch = 0;
  int tz = pc::clk::kTzDefault;
  pc::clk::Source source = pc::clk::Source::None;
  bool ntp_fresh = false;
  const char* reason = nullptr;
};

class WallClock {
 public:
  void set_hooks(ClockHooks hooks);
  void observe(int64_t now_s, int source, bool ntp_fresh, uint32_t now_ms);
  void restore_nvs(int64_t last, int tz);
  ClockEffect set_from_phone(int64_t epoch, bool has_tz, long tz, uint32_t now_ms);
  void note_ntp(int64_t epoch, uint32_t now_ms);
  int64_t now() const { return now_; }
  int64_t last_good() const { return last_good_; }
  bool have_last() const { return have_last_; }
  int tz() const { return tz_; }
  pc::clk::Source source() const { return src_; }
  bool signing_trusted() const;
  bool ntp_fresh_at(uint32_t now_ms) const;

 private:
  bool persist(int64_t epoch, bool have_epoch);
  ClockHooks hooks_{};
  int64_t now_ = 0;
  int64_t last_good_ = 0;
  bool have_last_ = false;
  int tz_ = pc::clk::kTzDefault;
  pc::clk::Source src_ = pc::clk::Source::None;
  bool ntp_ever_ = false;
  uint32_t ntp_at_ms_ = 0;
};

void wall_clock_bind(WallClock* clock);
WallClock* wall_clock_bound();

}  // namespace powercell
