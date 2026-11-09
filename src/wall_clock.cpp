#include "wall_clock.h"

namespace powercell {
namespace {
WallClock* bound_clock = nullptr;

pc::clk::Source source_from(int source) {
  if (source == 3) return pc::clk::Source::Ntp;
  if (source == 2) return pc::clk::Source::Phone;
  if (source == 1) return pc::clk::Source::Nvs;
  return pc::clk::Source::None;
}
}  // namespace

void wall_clock_bind(WallClock* clock) { bound_clock = clock; }
WallClock* wall_clock_bound() { return bound_clock; }

void WallClock::set_hooks(ClockHooks hooks) { hooks_ = hooks; }

void WallClock::observe(int64_t now_s, int source, bool ntp_fresh, uint32_t now_ms) {
  now_ = now_s;
  src_ = source_from(source);
  ntp_ever_ = ntp_fresh && src_ == pc::clk::Source::Ntp;
  ntp_at_ms_ = ntp_ever_ ? (now_ms ? now_ms : 1) : 0;
}

void WallClock::restore_nvs(int64_t last, int tz) {
  if (pc::clk::validTz(tz)) tz_ = static_cast<int>(tz);
  if (last < pc::clk::kMinEpoch || now_ >= pc::clk::kMinEpoch) return;
  now_ = last;
  src_ = pc::clk::Source::Nvs;
  last_good_ = last;
  have_last_ = true;
}

bool WallClock::persist(int64_t epoch, bool have_epoch) {
  if (have_epoch) {
    last_good_ = epoch;
    have_last_ = true;
  }
  if (!hooks_.persist) return true;
  return hooks_.persist(epoch, have_epoch, tz_, hooks_.ctx);
}

bool WallClock::signing_trusted() const {
  return now_ >= pc::clk::kMinEpoch && (src_ == pc::clk::Source::Ntp || src_ == pc::clk::Source::Phone);
}

bool WallClock::ntp_fresh_at(uint32_t now_ms) const {
  return ntp_ever_ && src_ == pc::clk::Source::Ntp &&
         static_cast<uint32_t>(now_ms - ntp_at_ms_) < pc::clk::kNtpFreshMs;
}

void WallClock::note_ntp(int64_t epoch, uint32_t now_ms) {
  if (epoch < pc::clk::kMinEpoch || epoch > pc::clk::kMaxEpoch) return;
  now_ = epoch;
  src_ = pc::clk::Source::Ntp;
  ntp_ever_ = true;
  ntp_at_ms_ = now_ms ? now_ms : 1;
  if (hooks_.write_rtc) hooks_.write_rtc(epoch, hooks_.ctx);
  persist(epoch, true);
}

ClockEffect WallClock::set_from_phone(int64_t epoch, bool has_tz, long tz, uint32_t now_ms) {
  ClockEffect out;
  if (has_tz && pc::clk::validTz(tz) && tz != tz_) {
    tz_ = static_cast<int>(tz);
    out.nvs_persisted = persist(have_last_ ? last_good_ : 0, have_last_);
  }
  const bool fresh = ntp_fresh_at(now_ms);
  const pc::clk::Verdict verdict = pc::clk::judge(epoch, now_, src_, fresh);
  out.verdict = verdict;
  out.tz = tz_;
  out.ntp_fresh = fresh;
  if (verdict == pc::clk::Verdict::Invalid) {
    out.reason = "invalid_epoch";
    out.epoch = now_;
    out.source = src_;
    out.rtc = "not_written";
    return out;
  }
  if (verdict != pc::clk::Verdict::Accept) {
    out.reason = pc::clk::verdictName(verdict);
    out.epoch = now_;
    out.source = src_;
    out.rtc = "not_written";
    out.applied = false;
    return out;
  }
  const int64_t previous = now_;
  const pc::clk::Source previous_src = src_;
  if (hooks_.set_system && !hooks_.set_system(epoch, hooks_.ctx)) {
    now_ = previous;
    src_ = previous_src;
    out.reason = "clock_set_failed";
    out.epoch = now_;
    out.source = src_;
    out.rtc = "not_written";
    out.system_set = false;
    return out;
  }
  now_ = epoch;
  src_ = pc::clk::Source::Phone;
  out.system_set = true;
  if (!hooks_.write_rtc) {
    out.rtc = "not_fitted";
  } else if (hooks_.write_rtc(epoch, hooks_.ctx)) {
    out.rtc = "persisted";
  } else {
    out.rtc = "failed";
  }
  out.nvs_persisted = persist(epoch, true);
  out.applied = true;
  out.epoch = now_;
  out.source = src_;
  out.ntp_fresh = false;
  out.reason = nullptr;
  return out;
}

}  // namespace powercell

int powercell_clock_signing(long long* now_out, int* source_out) {
  powercell::WallClock* clock = powercell::wall_clock_bound();
  if (!clock) return 0;
  if (now_out) *now_out = static_cast<long long>(clock->now());
  if (source_out) *source_out = static_cast<int>(clock->source());
  return clock->signing_trusted() ? 1 : 0;
}
