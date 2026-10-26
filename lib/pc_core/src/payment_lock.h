#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// HQ payment_lock signed command (backend resources/hq/payment_lock.md).
// Pure helpers: HMAC message layout, accept/reject after a verified signature,
// and the control-path refuse rule. Host tested.
namespace pc {
namespace paylock {

enum class Verdict : uint8_t { Apply, Duplicate, Stale, Invalid };

// Canonical HMAC message: cmd_id\n(locked?1:0)\nreason\nissued_at
// Writes without a trailing NUL into out; returns bytes written, or 0 if outCap is too small
// or the inputs are empty/oversized (cmd_id 1..64, reason 0..64).
inline size_t message(const char *cmdId, bool locked, const char *reason, uint32_t issuedAt,
                      char *out, size_t outCap) {
  if (!cmdId || !cmdId[0] || !out || !issuedAt) return 0;
  size_t idLen = strlen(cmdId);
  size_t rLen = reason ? strlen(reason) : 0;
  if (idLen > 64 || rLen > 64) return 0;
  char ts[12];
  int tn = snprintf(ts, sizeof ts, "%u", (unsigned)issuedAt);
  if (tn <= 0 || tn >= (int)sizeof ts) return 0;
  size_t need = idLen + 1 + 1 + 1 + rLen + 1 + (size_t)tn;
  if (need > outCap) return 0;
  char *p = out;
  memcpy(p, cmdId, idLen); p += idLen; *p++ = '\n';
  *p++ = locked ? '1' : '0'; *p++ = '\n';
  if (rLen) { memcpy(p, reason, rLen); p += rLen; }
  *p++ = '\n';
  memcpy(p, ts, (size_t)tn); p += tn;
  return (size_t)(p - out);
}

// After the signature verifies: apply when issued_at is newer than the last applied
// payment_lock and cmd_id is unseen. A replayed cmd_id is Duplicate (ack as done).
// issued_at <= lastApplied is Stale (server will resend). No 24h cutoff.
inline Verdict judge(uint32_t issuedAt, uint32_t lastAppliedAt, bool cmdSeen) {
  if (!issuedAt) return Verdict::Invalid;
  if (cmdSeen) return Verdict::Duplicate;
  if (issuedAt <= lastAppliedAt) return Verdict::Stale;
  return Verdict::Apply;
}

// User / consumer / cloud on-off and socket toggles are refused while locked.
// System origins (payment_lock force-off, battery_critical, ota) bypass at the call site.
inline bool controlBlocked(bool locked) { return locked; }

}  // namespace paylock
}  // namespace pc
