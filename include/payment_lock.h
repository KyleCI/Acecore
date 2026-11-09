#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// HQ payment_lock signed command from deployed 2.5.11.
// HMAC message layout and accept/reject after a verified signature.
namespace pc {
namespace paylock {

enum class Verdict : uint8_t { Apply, Duplicate, Stale, Invalid };

inline size_t message(const char* cmdId, bool locked, const char* reason, uint32_t issuedAt,
                      char* out, size_t outCap) {
  if (!cmdId || !cmdId[0] || !out || !issuedAt) return 0;
  size_t idLen = strlen(cmdId);
  size_t rLen = reason ? strlen(reason) : 0;
  if (idLen > 64 || rLen > 64) return 0;
  char ts[12];
  int tn = snprintf(ts, sizeof ts, "%u", (unsigned)issuedAt);
  if (tn <= 0 || tn >= (int)sizeof ts) return 0;
  size_t need = idLen + 1 + 1 + 1 + rLen + 1 + (size_t)tn;
  if (need > outCap) return 0;
  char* p = out;
  memcpy(p, cmdId, idLen);
  p += idLen;
  *p++ = '\n';
  *p++ = locked ? '1' : '0';
  *p++ = '\n';
  if (rLen) {
    memcpy(p, reason, rLen);
    p += rLen;
  }
  *p++ = '\n';
  memcpy(p, ts, (size_t)tn);
  p += tn;
  return (size_t)(p - out);
}

inline Verdict judge(uint32_t issuedAt, uint32_t lastAppliedAt, bool cmdSeen) {
  if (!issuedAt) return Verdict::Invalid;
  if (cmdSeen) return Verdict::Duplicate;
  if (issuedAt <= lastAppliedAt) return Verdict::Stale;
  return Verdict::Apply;
}

inline bool controlBlocked(bool locked) { return locked; }

}  // namespace paylock
}  // namespace pc
