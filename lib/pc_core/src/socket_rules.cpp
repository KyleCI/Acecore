#include "socket_rules.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace pc {
namespace sock {
bool parseHHMM(const char *s, uint16_t &min) {
  if (!s || strlen(s) != 5 || s[2] != ':') return false;
  static const int kd[4] = {0, 1, 3, 4};
  for (int i : kd) if (s[i] < '0' || s[i] > '9') return false;
  int h = (s[0] - '0') * 10 + s[1] - '0', m = (s[3] - '0') * 10 + s[4] - '0';
  if (h > 23 || m > 59) return false;
  min = (uint16_t)(h * 60 + m);
  return true;
}
void fmtHHMM(uint16_t min, char out[6]) { snprintf(out, 6, "%02u:%02u", (unsigned)(min / 60 % 24), (unsigned)(min % 60)); }
static const char *kModes[] = {"manual", "ai", "weather", "day_night"};
bool parseMode(const char *s, Mode &m) {
  for (uint8_t i = 0; s && i < 4; ++i) if (!strcmp(s, kModes[i])) { m = (Mode)i; return true; }
  return false;
}
const char *modeName(Mode m) { return kModes[(uint8_t)m & 3]; }
bool parsePrio(const char *s, Prio &p) {
  if (!s) return false;
  if (!strcmp(s, "essential")) { p = Prio::Essential; return true; }
  if (!strcmp(s, "heavy")) { p = Prio::Heavy; return true; }
  return false;
}
const char *prioName(Prio p) { return p == Prio::Essential ? "essential" : "heavy"; }
const char *reasonName(Reason r) {
  static const char *n[] = {nullptr, "safety_trip", "soc_heavy", "soc_essential", "inverter_overload", "cloud_override", "schedule", "day_night", "appliance_lockout", "manual"};
  return (uint8_t)r < 10 ? n[(uint8_t)r] : nullptr;
}
const char *fallbackName(Fallback f) { return f == Fallback::CloudOffline ? "cloud_offline" : f == Fallback::OverrideExpired ? "override_expired" : nullptr; }
bool limitDa(float amps, uint8_t &da, bool *clamped) {
  if (!isfinite(amps) || amps <= 0) return false;
  float d = roundf(amps * 10);
  bool c = d < kMinDa || d > kMaxDa;
  da = (uint8_t)(d < kMinDa ? kMinDa : d > kMaxDa ? kMaxDa : d);
  if (clamped) *clamped = c;
  return true;
}
bool validShed(int off, int on) { return off >= 0 && on <= 100 && off < on; }
uint32_t overrideMs(int64_t expiresAt, int64_t nowUnix, bool synced, int64_t ttl) {
  if (ttl <= 0 || ttl > kTtlS) ttl = kTtlS;
  int64_t s = ttl;
  if (expiresAt > 0 && synced) { int64_t left = expiresAt - nowUnix; if (left <= 0) return 0; if (left < s) s = left; }
  return (uint32_t)(s * 1000);
}
bool inWindow(const Sched &w, uint16_t m, uint8_t wday) {
  if (!w.mask) return false;
  uint8_t prev = (uint8_t)((wday + 6) % 7);
  if (w.start < w.end) return (w.mask >> wday & 1) && m >= w.start && m < w.end;
  if (w.start == w.end) return false;
  return ((w.mask >> wday & 1) && m >= w.start) || ((w.mask >> prev & 1) && m < w.end);  // wraps midnight
}
Mode effectiveMode(const Config &c, const State &s) { return s.fb != Fallback::None ? Mode::Manual : c.lastUserMode; }

Decision decide(const Config &c, State &s, const In &in) {
  Decision d;
  // Override expiry and the AI/Weather fallback.
  if (s.ov.set && (int32_t)(in.nowMs - s.ov.untilMs) >= 0) { s.ov.set = false; s.ovExpired = true; }
  bool cloudMode = c.lastUserMode == Mode::Ai || c.lastUserMode == Mode::Weather;
  s.fb = !cloudMode ? Fallback::None : !in.cloudUp ? Fallback::CloudOffline : s.ovExpired ? Fallback::OverrideExpired : Fallback::None;
  if (!in.cloudUp) s.ov.set = false;
  bool ess = c.prio == Prio::Essential;
  if (!in.gridDown) s.shed = false;  // SoC shedding only on battery (grid down or unknown)
  else if (in.socValid) {
    uint8_t off = ess ? c.essOff : c.heavyOff, on = ess ? c.essOn : c.heavyOn;
    s.shed = s.shed ? in.soc < on : in.soc <= off;
  }
  Reason shedR = ess ? Reason::SocEssential : Reason::SocHeavy;
  if (in.safety) { d.want = 0; d.reason = Reason::SafetyTrip; return d; }
  if (in.latched || s.shed) { d.want = 0; d.reason = shedR; return d; }
  if (in.overload && !ess) { d.want = 0; d.reason = Reason::InverterOverload; return d; }
  Mode m = effectiveMode(c, s);
  if ((m == Mode::Ai || m == Mode::Weather) && s.ov.set && !(s.ov.weather && ess)) {
    d.want = s.ov.on; d.reason = Reason::CloudOverride; return d;
  }
  if (!in.clockSynced) return d;  // schedules and Day/Night pause
  if (m == Mode::Manual) {
    for (uint8_t i = 0; i < c.nSched && i < kMaxSched; ++i)
      if (inWindow(c.sched[i], in.minutes, in.wday)) { d.want = c.sched[i].action ? 1 : 0; d.reason = Reason::Schedule; return d; }
  } else if (m == Mode::DayNight && !ess) {
    Sched night; night.mask = 0x7F; night.start = c.nightStart; night.end = c.dayStart;
    d.want = inWindow(night, in.minutes, in.wday) ? 0 : 1; d.reason = Reason::DayNight;
  }
  return d;
}
}  // namespace sock
}  // namespace pc
