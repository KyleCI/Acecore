#pragma once
// mDNS hostname and announce state for the local HTTP API (pure logic, host-tested).
// The ESPmDNS calls live in src/net.cpp.
#include <stddef.h>
#include <stdint.h>

namespace pc {
namespace mdns {
constexpr size_t kMaxLabel = 63;          // RFC 1035 label limit, without ".local"
constexpr size_t kHostCap = kMaxLabel + 1;
constexpr uint32_t kRetryMs = 30000;      // MDNS.begin failed: try again after this, or on the next got-IP

// One DNS label from a display id: letters, digits and '-' are kept (case kept),
// every run of other bytes becomes a single '-', leading/trailing '-' are dropped,
// and the result is cut to 63 chars (no trailing '-' after the cut).
// Returns the length; 0 (out = "") when nothing usable is left.
size_t hostLabel(const char *displayId, char *out, size_t cap);
// hostLabel(displayId), or "ACE-DDEEFF" from the MAC when the display id has no
// usable characters. Always non-empty when mac is given and cap >= 11.
size_t hostname(const char *displayId, const uint8_t mac[6], char *out, size_t cap);

// 2.5.20: the first mDNS start of a boot waits for the first WS TLS connect (or
// kTlsFirstMaxMs after the station got its IP), so the mDNS task stack, PCBs and
// records (~5 KB of heap) are not carved out of the block the first handshake needs.
// Never waits when the backend is disabled (no API key) or after a WS connect.
constexpr uint32_t kTlsFirstMaxMs = 60000;
inline bool waitForTls(bool backendEnabled, bool wsConnectedOnce, uint32_t ipAgeMs) {
  return backendEnabled && !wsConnectedOnce && ipAgeMs < kTlsFirstMaxMs;
}

// 2.5.30 heap rule (ACE-000003): the mDNS task (4 KB stack), PCBs, records and the
// multicast RX it attracts cost ~8-12 KB of DRAM. On 2.5.29 it was restarted on every
// got-IP, 0.5 s before the WS TLS reconnect (MDNS_UP 181309, TLS_FAILED -9984 181782),
// and with the WS up at free=17.6 KB MDNS.begin failed every 30 s (fragmenting churn).
//  - backend disabled: no WS to protect, always Ok.
//  - running: Release (stop it) when the WS is down and free DRAM is below the TLS floor,
//    so the reconnect gets that memory; otherwise keep it.
//  - not running: start only with a block >= kStartBlock and, WS up: free >= kStartFreeWsUp;
//    WS down: free >= tlsFloor + kHeapCost (the TLS floor still holds after the start).
constexpr uint32_t kHeapCost = 12288;
constexpr uint32_t kStartFreeWsUp = 32768;
constexpr uint32_t kStartBlock = 8192;
constexpr uint32_t kDeferLogMs = 300000;   // MDNS_DEFERRED at most every 5 min
enum class HeapRule : uint8_t { Ok, Defer, Release };
inline HeapRule heapRule(bool running, bool backendEnabled, bool wsUp, uint32_t freeB, uint32_t largest, uint32_t tlsFloor) {
  if (!backendEnabled) return HeapRule::Ok;
  if (running) return (!wsUp && freeB < tlsFloor) ? HeapRule::Release : HeapRule::Ok;
  if (largest < kStartBlock) return HeapRule::Defer;
  if (wsUp) return freeB >= kStartFreeWsUp ? HeapRule::Ok : HeapRule::Defer;
  return freeB >= tlsFloor + kHeapCost ? HeapRule::Ok : HeapRule::Defer;
}
enum class Action : uint8_t { None, Start, Restart, Stop };

// Decides when to (re)announce. allowed = station has an IP on the saved home
// network (not hotspot-only, no Wi-Fi test running or awaiting save).
// gotIpGen is bumped by the STA_GOT_IP event, so every reconnect re-announces.
struct Announcer {
  bool running = false;
  bool failed = false;
  uint32_t seenGen = 0;
  uint32_t failAt = 0;
  char host[kHostCap] = {0};

  Action decide(bool allowed, uint32_t gotIpGen, const char *wantHost, uint32_t now) const;
  void started(bool ok, const char *hostName, uint32_t gotIpGen, uint32_t now);
  void stopped();
};
}  // namespace mdns
}  // namespace pc
