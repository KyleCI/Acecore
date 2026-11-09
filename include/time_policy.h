#pragma once
#include <stdint.h>
// POST /v2/time policy from deployed 2.5.11. Phone time is accepted only when
// the clock is unsynced or more than 60 seconds off. Fresh NTP stays authoritative.
namespace pc {
namespace clk {
enum class Source : uint8_t { None, Nvs, Phone, Ntp };
enum class Verdict : uint8_t { Accept, InSync, NtpAuthoritative, Invalid };
constexpr int64_t kMinEpoch = 1700000000, kMaxEpoch = 4102444800LL;
constexpr int kTzMin = -720, kTzMax = 840, kTzDefault = 60;
constexpr int64_t kMaxSkewS = 60;
constexpr uint32_t kNtpFreshMs = 2u * 3600u * 1000u;
inline bool validTz(long tz) { return tz >= kTzMin && tz <= kTzMax; }
inline Verdict judge(int64_t epoch, int64_t now, Source src, bool ntpFresh) {
  if (epoch < kMinEpoch || epoch > kMaxEpoch) return Verdict::Invalid;
  if (src == Source::Ntp && ntpFresh) return Verdict::NtpAuthoritative;
  bool unsynced = src == Source::None || src == Source::Nvs || now < kMinEpoch;
  if (unsynced) return Verdict::Accept;
  int64_t d = epoch - now;
  if (d < 0) d = -d;
  return d > kMaxSkewS ? Verdict::Accept : Verdict::InSync;
}
inline const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::Accept: return "applied";
    case Verdict::InSync: return "clock_in_sync";
    case Verdict::NtpAuthoritative: return "ntp_authoritative";
    default: return "invalid_epoch";
  }
}
inline const char* sourceName(Source s) {
  switch (s) {
    case Source::Nvs: return "nvs";
    case Source::Phone: return "phone";
    case Source::Ntp: return "ntp";
    default: return "none";
  }
}
}  // namespace clk
}  // namespace pc
