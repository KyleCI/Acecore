#pragma once
#include <stdint.h>
namespace pc {
// TLS memory guard: a TLS handshake needs one contiguous block of about 40 KB.
// Before any TLS connect (WS or OTA) the largest free block must be >= kTlsMinBlock;
// otherwise back off 5 s, 10 s, 20 s, 40 s, 60 s (cap) and log HEAP_LOW once per backoff.
// v12: 40 KB. mbedTLS here allocates IN and OUT records of ~16.7 KB each
// (MBEDTLS_SSL_MAX_CONTENT_LEN 16384 is fixed in the precompiled Arduino libs) plus
// handshake/cert scratch; the largest single allocation is ~17 KB, so 40 KB contiguous
// leaves >2x headroom. 45 KB rejected heaps that could connect (42996 after onboarding).
constexpr uint32_t kTlsMinBlock = 40960;
class TlsBackoff {
 public:
  enum Verdict : uint8_t { Go, Wait, WaitLog };  // WaitLog = a new backoff started: log HEAP_LOW
  Verdict check(uint32_t now, uint32_t largest) {
    if (delay_ && (int32_t)(now - next_) < 0) return Wait;
    if (largest >= kTlsMinBlock) { delay_ = 0; return Go; }
    delay_ = delay_ ? (delay_ * 2 > kMax ? kMax : delay_ * 2) : kFirst;
    next_ = now + delay_;
    return WaitLog;
  }
  uint32_t delayMs() const { return delay_; }
  static constexpr uint32_t kFirst = 5000, kMax = 60000;
 private:
  uint32_t next_ = 0, delay_ = 0;
};

// OTA check transport (v11).
// - Once any hello_ack this boot said ota_ws=true, checks go over the WS only: a WS
//   check that times out, or a WS that is down when a check is due, defers the check
//   until the WS has been reconnected for kSettleMs. No HTTPS fallback.
// - HTTPS fallback only when ota_ws was never true this boot and kFallbackCycles
//   consecutive WS connections came and went (or are up) without ota_ws=true
//   (hello_ack said false, or said nothing).
class OtaTransport {
 public:
  enum Support : uint8_t { Unknown, Yes, No };
  enum Plan : uint8_t { Wait, Ws, Https };
  static constexpr uint32_t kWsReplyMs = 20000, kSettleMs = 30000;
  static constexpr uint8_t kFallbackCycles = 3;
  void helloAck(bool otaWs) {
    support_ = otaWs ? Yes : No;
    if (otaWs) { seenYes_ = true; cyclesNoWs_ = 0; }
  }
  // each WS (re)connection; counts toward the fallback until ota_ws=true is seen
  void wsConnected() { support_ = Unknown; if (!seenYes_ && cyclesNoWs_ < 255) ++cyclesNoWs_; }
  void wsDown() { waiting_ = false; }  // an outstanding check died with the WS: deferred
  Support support() const { return support_; }
  bool seenYes() const { return seenYes_; }
  bool httpsAllowed() const { return !seenYes_ && cyclesNoWs_ >= kFallbackCycles; }
  Plan plan(bool wsConnected, uint32_t connectedForMs) const {
    if (httpsAllowed()) return Https;
    if (!wsConnected || connectedForMs < kSettleMs) return Wait;
    return Ws;
  }
  bool useWs(bool wsConnected) const { return wsConnected && !httpsAllowed(); }
  void sent(uint32_t now) { waiting_ = true; sentAt_ = now; }
  // true exactly once when the reply window expired: the caller retries (WS) later.
  bool timedOut(uint32_t now) {
    if (waiting_ && now - sentAt_ >= kWsReplyMs) { waiting_ = false; return true; }
    return false;
  }
  // a reply counts only while we are waiting for one (late/unsolicited replies are ignored)
  bool reply() { if (!waiting_) return false; waiting_ = false; support_ = Yes; seenYes_ = true; cyclesNoWs_ = 0; return true; }
  bool waiting() const { return waiting_; }
  void cancel() { waiting_ = false; }
 private:
  Support support_ = Unknown;
  bool seenYes_ = false, waiting_ = false;
  uint8_t cyclesNoWs_ = 0;
  uint32_t sentAt_ = 0;
};
}  // namespace pc
