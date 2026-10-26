#pragma once
#include <stddef.h>
#include <stdint.h>
// AcePlug socket config (contract: official-powercell-app PR #8, backend/docs/SOCKET_CONFIG.md,
// socket_command_contract.json 1.0.10). Pure logic, host tested. Per-socket config is stored in
// NVS as one blob (sizeof(Config), about 90 B). Local engine, highest priority first:
//   1. safety (ESP backup monitor: current above the user limit / 20 A with the relay still on)
//   2. SoC shedding (per-socket thresholds, priority essential|heavy) and the battery critical latch
//   3. inverter overload (heavy sockets)
//   4. cloud override (ai|weather) until it expires; only while the mode is ai or weather
//   5. schedules (mode manual) or Day/Night (mode day_night, heavy only), local time; paused unsynced
//   6. manual (no automatic command)
// AI/Weather fall back to manual while the cloud link is down or after an override expires.
namespace pc {
namespace sock {
enum class Mode : uint8_t { Manual, Ai, Weather, DayNight };
enum class Prio : uint8_t { Heavy, Essential };
enum class Reason : uint8_t { None, SafetyTrip, SocHeavy, SocEssential, InverterOverload, CloudOverride, Schedule, DayNight, ApplianceLockout, Manual };
enum class Fallback : uint8_t { None, CloudOffline, OverrideExpired };
constexpr uint8_t kMaxSched = 8, kMaxSockets = 10, kNameMax = 24;
constexpr uint8_t kMinDa = 10, kMaxDa = 200;  // limit in deciamps: 1.0 .. 20.0 A
constexpr uint8_t kTripAmps = 20;             // fixed hard trip, not a setting
constexpr uint32_t kTtlS = 1800;              // override ttl default and cap
constexpr uint32_t kCloudGraceMs = 60000;     // cloud down this long before the AI/Weather fallback

struct Sched { uint8_t mask = 0, action = 0; uint16_t start = 0, end = 0; };  // mask bit0 Sunday; minutes
struct Config {
  uint8_t ver = 1;
  char name[kNameMax + 1] = {};
  uint8_t limitDa = kMaxDa;
  Mode mode = Mode::Manual, lastUserMode = Mode::Manual;
  Prio prio = Prio::Heavy;
  uint16_t dayStart = 9 * 60, nightStart = 21 * 60;
  uint8_t nSched = 0;
  Sched sched[kMaxSched];
  uint8_t heavyOff = 40, heavyOn = 50, essOff = 20, essOn = 30;
};
static_assert(sizeof(Config) <= 96, "socket config blob");

struct Override { bool set = false, on = false, weather = false; uint32_t untilMs = 0; };

bool parseHHMM(const char *s, uint16_t &min);
void fmtHHMM(uint16_t min, char out[6]);
bool parseMode(const char *s, Mode &m);
const char *modeName(Mode m);
bool parsePrio(const char *s, Prio &p);
const char *prioName(Prio p);
const char *reasonName(Reason r);    // nullptr for None
const char *fallbackName(Fallback f);  // nullptr for None
// amps -> deciamps, clamped to 1..20 A. *clamped set when the input was outside. false: not a number > 0.
bool limitDa(float amps, uint8_t &da, bool *clamped);
bool validShed(int off, int on);  // 0 <= off < on <= 100
// Override deadline in ms from now: expires_at (when the clock is synced) capped by ttl (<= 1800 s).
// 0 = already expired / invalid.
uint32_t overrideMs(int64_t expiresAt, int64_t nowUnix, bool clockSynced, int64_t ttlS);

struct In {
  bool socValid = false; float soc = 0;
  bool gridDown = true;      // grid below 160 V (restore 170 V), or unknown: SoC shedding only then
  bool latched = false;      // battery critical (ESP 10 % latch or STM32 reserve)
  bool overload = false;     // inverter overload
  bool safety = false;       // ESP backup monitor tripped this socket
  bool cloudUp = true;       // backend link up (after the grace)
  bool clockSynced = false; uint16_t minutes = 0; uint8_t wday = 0;  // local time, wday 0 = Sunday
  uint32_t nowMs = 0;
};
struct State {
  bool shed = false;
  Override ov;
  bool ovExpired = false;
  Fallback fb = Fallback::None;
};
struct Decision { int8_t want = -1; Reason reason = Reason::None; };  // want: -1 none, 0 off, 1 on
Mode effectiveMode(const Config &c, const State &s);
// Updates the shed hysteresis, override expiry and fallback in s.
Decision decide(const Config &c, State &s, const In &in);
// Schedule window match: [start, end), end <= start wraps past midnight (day of the start).
bool inWindow(const Sched &w, uint16_t minutes, uint8_t wday);
}  // namespace sock
}  // namespace pc
