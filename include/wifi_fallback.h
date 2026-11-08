#pragma once

#include <stdint.h>

namespace powercell {

// Local-mode timing for the collector. This source previously blocked setup
// for CONN_TIMEOUT (10s) and then called WiFi.disconnect(true). It had no
// runtime fallback after an established link dropped. The deployed 2.5.11
// report (about 301s, AP_FALLBACK_MIN=5) is not this tree.
//
// loss_to_local_ms is the deadline from confirmed loss, or from boot if the
// station never confirms. recovery_ap_grace_ms holds the AP after the station
// returns. It does not delay the 30s fallback. There was no 120s constant in
// this tree; 120s is only the post-recovery hold.
struct WifiFallbackConfig {
  uint32_t loss_to_local_ms = 30000;
  uint32_t confirm_up_ms = 3000;
  uint32_t recovery_ap_grace_ms = 120000;
  uint32_t reconnect_interval_ms = 5000;
};

enum class WifiSta : uint8_t { Down = 0, Up = 1 };

struct WifiCommand {
  bool start_ap = false;
  bool stop_ap = false;
  bool reconnect_sta = false;
  bool local_mode = false;
};

class WifiFallback {
 public:
  explicit WifiFallback(WifiFallbackConfig config = WifiFallbackConfig());

  void set_credentials(bool have);
  void set_onboarding(bool active);
  WifiCommand tick(WifiSta sta, uint32_t now_ms);

  bool local_mode() const { return local_; }
  bool ap_on() const { return ap_on_; }
  bool confirmed() const { return confirmed_; }
  uint32_t reconnects() const { return reconnects_; }
  uint32_t ap_starts() const { return ap_starts_; }
  uint32_t ap_stops() const { return ap_stops_; }

 private:
  static uint32_t elapsed(uint32_t now, uint32_t since) { return now - since; }

  WifiFallbackConfig cfg_;
  bool have_credentials_ = false;
  bool onboarding_ = false;
  bool started_ = false;
  bool confirmed_ = false;
  bool up_streak_ = false;
  bool loss_armed_ = false;
  bool local_ = false;
  bool ap_on_ = false;
  bool grace_armed_ = false;
  uint32_t up_since_ = 0;
  uint32_t loss_since_ = 0;
  uint32_t grace_until_ = 0;
  uint32_t last_reconnect_ = 0;
  uint32_t reconnects_ = 0;
  uint32_t ap_starts_ = 0;
  uint32_t ap_stops_ = 0;
};

}  // namespace powercell
