#pragma once
// PZEM poll counters and a rate-limited failure warning. Pure logic, host-tested.
#include <stddef.h>
#include <stdint.h>
#include "pzem_v3.h"

namespace powercell {

enum class PzemPollOutcome : uint8_t { Ok = 0, NoReply, Short, Bad };

// got == 0 is no reply at all (wiring, pull-up on RX, PZEM unpowered).
// 0 < got < 25 is a cut-off frame. 25 bytes that do not parse is Bad.
inline PzemPollOutcome pzem_classify(size_t got, PzemStatus status, bool sample_ok) {
  if (sample_ok && status == PzemStatus::Ok) return PzemPollOutcome::Ok;
  if (got == 0) return PzemPollOutcome::NoReply;
  if (got < 25) return PzemPollOutcome::Short;
  return PzemPollOutcome::Bad;
}

struct PzemStats {
  static constexpr uint32_t kWarnEveryMs = 30000;
  uint32_t polls = 0, ok = 0, no_reply = 0, short_frames = 0, bad_frames = 0;
  uint32_t consecutive_fail = 0;
  uint32_t last_ok_ms = 0;
  bool ever_ok = false;
  bool failing = false;       // a warning has been logged for this episode
  uint32_t last_warn_ms = 0;

  enum class Log : uint8_t { None = 0, Warn, Recovered };

  // Returns what to log for this poll. The first failure logs at once, then at
  // most once per kWarnEveryMs while it keeps failing. One Recovered on the
  // first good poll after a warning.
  Log record(PzemPollOutcome o, uint32_t now) {
    ++polls;
    if (o == PzemPollOutcome::Ok) {
      ++ok;
      consecutive_fail = 0;
      last_ok_ms = now;
      ever_ok = true;
      if (failing) { failing = false; return Log::Recovered; }
      return Log::None;
    }
    if (o == PzemPollOutcome::NoReply) ++no_reply;
    else if (o == PzemPollOutcome::Short) ++short_frames;
    else ++bad_frames;
    ++consecutive_fail;
    if (!failing || now - last_warn_ms >= kWarnEveryMs) {
      failing = true;
      last_warn_ms = now;
      return Log::Warn;
    }
    return Log::None;
  }
};

inline const char* pzem_outcome_name(PzemPollOutcome o) {
  switch (o) {
    case PzemPollOutcome::Ok: return "ok";
    case PzemPollOutcome::NoReply: return "no_reply";
    case PzemPollOutcome::Short: return "short";
    default: return "bad";
  }
}

}  // namespace powercell
