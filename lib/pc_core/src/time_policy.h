#pragma once
#include <stdint.h>
// POST /v2/time policy (Caleb, Oct 22 2026). Pure, host tested.
// Phone time is accepted only when the clock is unsynced (no time, or only the NVS
// last-good restore) or more than 60 s off. NTP stays authoritative: while the clock
// came from NTP and NTP synced within the last 2 h, phone time is refused.
namespace pc {
namespace clk {
enum class Source : uint8_t { None, Nvs, Phone, Ntp, Rtc };
enum class Verdict : uint8_t { Accept, InSync, NtpAuthoritative, Invalid };
constexpr int64_t kMinEpoch = 1700000000, kMaxEpoch = 4102444800LL;  // 2023-11 .. 2100
constexpr int kTzMin = -720, kTzMax = 840, kTzDefault = 60;          // minutes; default WAT +60
constexpr int64_t kMaxSkewS = 60;
constexpr uint32_t kNtpFreshMs = 2u * 3600u * 1000u;
inline bool validTz(long tz) { return tz >= kTzMin && tz <= kTzMax; }
inline Verdict judge(int64_t epoch, int64_t now, Source src, bool ntpFresh) {
  if (epoch < kMinEpoch || epoch > kMaxEpoch) return Verdict::Invalid;
  if (src == Source::Ntp && ntpFresh) return Verdict::NtpAuthoritative;
  bool unsynced = src == Source::None || src == Source::Nvs || now < kMinEpoch;
  if (unsynced) return Verdict::Accept;
  int64_t d = epoch - now; if (d < 0) d = -d;
  return d > kMaxSkewS ? Verdict::Accept : Verdict::InSync;
}
// 2.5.39: clock good enough to run socket schedules. NTP and phone time always; the DS3231 RTC
// only where it is fitted and trusted (DSP v0.1.1): it is read once at boot, only accepted when
// coherent (OSF clear, valid calendar) and is only ever written from NTP or phone time. The NVS
// last-good restore is stale by the downtime and never runs a schedule. Epoch must be 2023..2100.
inline bool scheduleClockValid(Source s, int64_t epoch, bool rtcTrusted) {
  if (epoch < kMinEpoch || epoch > kMaxEpoch) return false;
  return s == Source::Ntp || s == Source::Phone || (rtcTrusted && s == Source::Rtc);
}
inline const char *verdictName(Verdict v) {
  switch (v) { case Verdict::Accept: return "applied"; case Verdict::InSync: return "clock_in_sync";
    case Verdict::NtpAuthoritative: return "ntp_authoritative"; default: return "invalid_epoch"; }
}
inline const char *sourceName(Source s) {
  switch (s) { case Source::Nvs: return "nvs"; case Source::Phone: return "phone"; case Source::Ntp: return "ntp"; case Source::Rtc: return "rtc"; default: return "none"; }
}
}  // namespace clk
}  // namespace pc
