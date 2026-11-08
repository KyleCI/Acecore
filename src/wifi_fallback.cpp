#include "wifi_fallback.h"

namespace powercell {

WifiFallback::WifiFallback(WifiFallbackConfig config) : cfg_(config) {}

void WifiFallback::set_credentials(bool have) { have_credentials_ = have; }

void WifiFallback::set_onboarding(bool active) { onboarding_ = active; }

WifiCommand WifiFallback::tick(WifiSta sta, uint32_t now_ms) {
  WifiCommand cmd;
  if (!started_) {
    started_ = true;
    loss_armed_ = true;
    loss_since_ = now_ms;
    last_reconnect_ = now_ms - cfg_.reconnect_interval_ms;
  }

  if (!have_credentials_) {
    local_ = true;
    confirmed_ = false;
    if (!ap_on_) {
      cmd.start_ap = true;
      ap_on_ = true;
      ++ap_starts_;
    }
    cmd.local_mode = true;
    return cmd;
  }

  if (sta == WifiSta::Up) {
    if (!up_streak_) {
      up_streak_ = true;
      up_since_ = now_ms;
    }
    if (!confirmed_ && elapsed(now_ms, up_since_) >= cfg_.confirm_up_ms) {
      confirmed_ = true;
      if (ap_on_ || local_) {
        grace_armed_ = true;
        grace_until_ = now_ms + cfg_.recovery_ap_grace_ms;
      }
      local_ = false;
    }
  } else {
    up_streak_ = false;
    if (confirmed_) {
      confirmed_ = false;
      loss_since_ = now_ms;
      loss_armed_ = true;
    } else if (!loss_armed_) {
      loss_since_ = now_ms;
      loss_armed_ = true;
    }
  }

  if (!confirmed_ && loss_armed_ && elapsed(now_ms, loss_since_) >= cfg_.loss_to_local_ms) {
    local_ = true;
    if (!ap_on_) {
      cmd.start_ap = true;
      ap_on_ = true;
      ++ap_starts_;
    }
  }

  if (!confirmed_) {
    if (elapsed(now_ms, last_reconnect_) >= cfg_.reconnect_interval_ms) {
      cmd.reconnect_sta = true;
      last_reconnect_ = now_ms;
      ++reconnects_;
    }
  }

  const bool grace_done = !grace_armed_ || static_cast<int32_t>(now_ms - grace_until_) >= 0;
  if (ap_on_ && confirmed_ && !local_ && !onboarding_ && grace_done) {
    cmd.stop_ap = true;
    ap_on_ = false;
    grace_armed_ = false;
    ++ap_stops_;
  }

  cmd.local_mode = local_;
  return cmd;
}

}  // namespace powercell
