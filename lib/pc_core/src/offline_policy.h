#pragma once
#include <stdint.h>
#include <string.h>
// Offline queue replay rules (2.5.31, host tested). ACE-000003: a notification record
// ("ev_over_temp", queued by events.cpp while the WS was down) sat in the NVS offline queue and,
// once the WS TLS session came up on 2.5.29, was re-sent in every offline_sync batch on the 60 s
// backoff cap because no offline_sync_ack removed it. The backend turned each resend into an
// "Inverter too hot" alert (and a clear on the next telemetry without it in active_events).
//
// Rules:
//  * ev_* records (event notifications) are best effort: after kEventMaxUnackedSends batches
//    with no offline_sync_ack they are dropped. User actions (gateway_state, socket_state,
//    socket_reset_lockout) and socket_* lifecycle records keep the old retry-until-ack rule.
//  * ev_* records restored from NVS (an earlier boot) are dropped at boot: they are stale
//    alerts, and the live state reaches the backend in active_events.
namespace pc {
namespace offq {
constexpr uint8_t kEventMaxUnackedSends = 3;  // first send + 2 retries (2 s, 4 s backoff)
inline bool isEventRecord(const char *action) { return action && !strncmp(action, "ev_", 3); }
inline bool dropOnRestore(const char *action) { return isEventRecord(action); }
class ResendGuard {
 public:
  void sent() { if (unacked_ < 255) ++unacked_; }
  void acked() { unacked_ = 0; }
  uint8_t unacked() const { return unacked_; }
  // True when the ev_* records in the unacked batch should be dropped before the next send.
  bool dropEvents() const { return unacked_ >= kEventMaxUnackedSends; }
 private:
  uint8_t unacked_ = 0;
};
// Remove the ev_* records among the first batchMax entries (the batch that went unacked).
// Q is a pc::RingQueue of records with a char action[] field. Order is kept.
template <class Q>
size_t dropUnackedEvents(Q &q, size_t batchMax) {
  size_t idx = 0;
  return q.removeIf([&](const typename Q::value_type &e) { return idx++ < batchMax && isEventRecord(e.action); });
}
}  // namespace offq
}  // namespace pc
