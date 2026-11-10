#pragma once
#include <stdint.h>
namespace pc {
// Who owns the network during an OTA cycle. The periodic CHECK is a small HTTP
// request that runs in the OTA worker while the backend WebSocket keeps
// running (ws.loop() keeps answering pings). Only an actual INSTALL pauses the
// WebSocket, and only once the main loop has granted it (no socket command in
// flight). Before this split, every 5-min check (and the first one 35 s after
// boot) disconnected the WebSocket and stopped ws.loop() for the whole check.
class OtaGate {
 public:
  enum Phase : uint8_t { Idle, Checking, WantInstall, Installing };
  explicit OtaGate(uint32_t grantTimeoutMs = 60000) : timeout_(grantTimeoutMs) {}
  Phase phase() const { return phase_; }
  bool start() { if (phase_ != Idle) return false; phase_ = Checking; return true; }   // loop
  void requestInstall(uint32_t now) { if (phase_ == Checking) { phase_ = WantInstall; since_ = now; } }  // worker
  // loop: call while WantInstall; `paused` = the WebSocket was released.
  void grant(bool paused) { if (phase_ == WantInstall && paused) phase_ = Installing; }
  // worker: true once it may install; false (and back to Checking) after the grant timeout.
  bool mayInstall(uint32_t now) {
    if (phase_ == Installing) return true;
    if (phase_ == WantInstall && now - since_ > timeout_) phase_ = Checking;
    return false;
  }
  bool waiting() const { return phase_ == WantInstall; }
  void finish() { phase_ = Idle; }                        // worker, on every exit path
  bool wsPaused() const { return phase_ == Installing; }   // backend must not touch the socket
  bool busy() const { return phase_ != Idle; }
 private:
  volatile Phase phase_ = Idle;
  uint32_t since_ = 0, timeout_;
};
}  // namespace pc
