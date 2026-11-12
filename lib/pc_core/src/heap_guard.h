#pragma once
#include <stdint.h>
namespace pc {
// TLS memory guard (2.5.20). Evaluated before a NEW TLS connect (WS or OTA) only.
//
// What the handshake really allocates (Arduino-ESP32 2.0.17 / IDF 4.4.7 / mbedTLS 2.28,
// precompiled libs; sdkconfig: CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 16384, no
// CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN, no CONFIG_MBEDTLS_DYNAMIC_BUFFER, no
// CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH, CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC):
//  - mbedtls_ssl_setup() callocs the IN and OUT record buffers separately, each
//    MBEDTLS_SSL_IN/OUT_BUFFER_LEN = 13 header + 16384 content + 320 payload overhead
//    (16 IV + 48 SHA-384 MAC + 256 CBC padding, no compression, no DTLS CID) = 16717 B.
//    Plus 4 B multi_heap header and 12 B CONFIG_HEAP_POISONING_LIGHT canaries = 16736 B.
//    These two are the only large allocations; everything else (ssl/config/handshake
//    contexts, CA and peer chain, ECDHE/RSA bignums) is many blocks of < 2 KB, about
//    12-16 KB in total at the handshake peak.
//  - The IDF 4.4 allocator is best-fit per heap, so the two record buffers need either
//    one block >= 2 x 16736, or two blocks >= 16736 each.
// Rules:
//  - free heap >= kTlsMinFree: both record buffers (33.5 KB) + ~13 KB handshake peak
//    + ~4 KB for Wi-Fi/lwIP RX during the handshake. v10 field evidence: a second
//    handshake with the WS session up (heap_min ~44 KB) failed in mbedTLS with -9984
//    (X509 verify failed: out of memory in the chain check, see ota.cpp).
//  - largest block >= kTlsPairBlock: both record buffers fit in it: go.
//  - largest block in [kTlsMinBlock, kTlsPairBlock): the caller probes by allocating the
//    two record buffers for real (same caps as mbedTLS) and freeing them: go if both fit.
//  - largest block < kTlsMinBlock: no record buffer fits anywhere: wait.
// History: v12..2.5.19 used one fixed 40960 B block threshold. ACE-000001 on 2.5.18
// (12 Nov 2026) sat at largest=40948 free=72064, 12 B short, and never connected,
// although 40948 B holds both record buffers.
constexpr uint32_t kTlsRecordBuf = 16717;    // MBEDTLS_SSL_IN_BUFFER_LEN == MBEDTLS_SSL_OUT_BUFFER_LEN
constexpr uint32_t kHeapAllocOverhead = 16;  // multi_heap block header 4 + light poisoning 12
constexpr uint32_t kTlsRecordAlloc = (kTlsRecordBuf + kHeapAllocOverhead + 3u) & ~3u;  // 16736
constexpr uint32_t kTlsMinBlock = 18432;                         // one record buffer + 1.7 KB
constexpr uint32_t kTlsPairBlock = 2 * kTlsRecordAlloc + 2048;  // 35520: both buffers + 2 KB
constexpr uint32_t kTlsMinFree = 51200;                          // 50 KB total
static_assert(kTlsRecordAlloc == 16736, "record buffer allocation size");
static_assert(kTlsMinBlock > kTlsRecordAlloc && kTlsPairBlock > 2 * kTlsRecordAlloc, "gate covers the record buffers");
static_assert(kTlsMinFree > 2 * kTlsRecordAlloc + 12288, "free floor covers buffers + handshake");

enum class TlsHeap : uint8_t { Ok, Probe, LowFree, LowBlock };
inline TlsHeap tlsHeapVerdict(uint32_t largest, uint32_t freeBytes) {
  if (freeBytes < kTlsMinFree) return TlsHeap::LowFree;
  if (largest < kTlsMinBlock) return TlsHeap::LowBlock;
  if (largest >= kTlsPairBlock) return TlsHeap::Ok;
  return TlsHeap::Probe;
}
inline const char *tlsHeapName(TlsHeap v) {
  switch (v) {
    case TlsHeap::Ok: return "ok";
    case TlsHeap::Probe: return "probe";
    case TlsHeap::LowFree: return "low_free";
    case TlsHeap::LowBlock: return "low_block";
  }
  return "?";
}
// Full decision. probe() allocates and frees the two record buffers (device glue);
// it is called only for TlsHeap::Probe. Returns true = go.
template <typename ProbeFn>
bool tlsHeapOk(uint32_t largest, uint32_t freeBytes, ProbeFn probe, TlsHeap *why = nullptr) {
  TlsHeap v = tlsHeapVerdict(largest, freeBytes);
  bool ok = v == TlsHeap::Ok || (v == TlsHeap::Probe && probe());
  if (why) *why = v;
  return ok;
}

// Backoff after a failed guard: 5 s, 10 s, 20 s, 40 s, 60 s (cap); HEAP_LOW is logged
// once per backoff step (WaitLog). A pass resets it.
class TlsBackoff {
 public:
  enum Verdict : uint8_t { Go, Wait, WaitLog };  // WaitLog = a new backoff started: log HEAP_LOW
  // True when the guard should be evaluated now (no backoff running or it expired).
  bool due(uint32_t now) const { return !delay_ || (int32_t)(now - next_) >= 0; }
  Verdict step(uint32_t now, bool heapOk) {
    if (!due(now)) return Wait;
    if (heapOk) { delay_ = 0; return Go; }
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
