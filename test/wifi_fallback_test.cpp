#include "wifi_fallback.h"

#include <stdio.h>

using namespace powercell;

static int g_failures = 0;

#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
  } \
} while (0)

static void boot_offline_reaches_local_at_30s() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  CHECK(!wifi.tick(WifiSta::Down, 0).start_ap);
  CHECK(!wifi.tick(WifiSta::Down, 29999).start_ap);
  CHECK(!wifi.local_mode());
  WifiCommand at = wifi.tick(WifiSta::Down, 30000);
  CHECK(at.start_ap);
  CHECK(at.local_mode);
  CHECK(!at.stop_ap);
  CHECK(wifi.ap_starts() == 1);
  CHECK(!wifi.tick(WifiSta::Down, 60000).start_ap);
  CHECK(wifi.ap_starts() == 1);
  CHECK(wifi.ap_stops() == 0);
}

static void boot_without_credentials_is_immediate() {
  WifiFallback wifi;
  wifi.set_credentials(false);
  WifiCommand cmd = wifi.tick(WifiSta::Down, 0);
  CHECK(cmd.start_ap);
  CHECK(cmd.local_mode);
  CHECK(!cmd.reconnect_sta);
  CHECK(!cmd.stop_ap);
  CHECK(wifi.reconnects() == 0);
}

static void established_loss_waits_30s_from_the_drop() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  CHECK(wifi.tick(WifiSta::Up, 0).reconnect_sta);
  CHECK(!wifi.confirmed());
  CHECK(wifi.tick(WifiSta::Up, 2999).reconnect_sta == false || !wifi.confirmed());
  wifi.tick(WifiSta::Up, 3000);
  CHECK(wifi.confirmed());
  CHECK(!wifi.ap_on());
  CHECK(!wifi.tick(WifiSta::Down, 10000).start_ap);
  CHECK(!wifi.tick(WifiSta::Down, 39999).start_ap);
  WifiCommand cmd = wifi.tick(WifiSta::Down, 40000);
  CHECK(cmd.start_ap);
  CHECK(cmd.local_mode);
  CHECK(!cmd.stop_ap);
}

static void recovery_keeps_ap_for_120s_then_stops() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  wifi.tick(WifiSta::Down, 0);
  CHECK(wifi.tick(WifiSta::Down, 30000).start_ap);
  wifi.tick(WifiSta::Up, 50000);
  CHECK(!wifi.confirmed());
  wifi.tick(WifiSta::Up, 53000);
  CHECK(wifi.confirmed());
  CHECK(wifi.ap_on());
  CHECK(!wifi.tick(WifiSta::Up, 53000 + 119999).stop_ap);
  CHECK(wifi.ap_on());
  CHECK(wifi.tick(WifiSta::Up, 53000 + 120000).stop_ap);
  CHECK(!wifi.ap_on());
  CHECK(wifi.ap_stops() == 1);
}

static void flapping_does_not_postpone_fallback_or_churn_ap() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  wifi.tick(WifiSta::Up, 0);
  wifi.tick(WifiSta::Up, 3000);
  CHECK(wifi.confirmed());
  wifi.tick(WifiSta::Down, 10000);
  for (uint32_t t = 12000; t < 40000; t += 2000) {
    wifi.tick(WifiSta::Up, t);
    wifi.tick(WifiSta::Down, t + 1000);
  }
  CHECK(!wifi.confirmed());
  CHECK(!wifi.ap_on());
  wifi.tick(WifiSta::Down, 40000);
  CHECK(!wifi.confirmed());
  CHECK(wifi.ap_on());
  CHECK(wifi.ap_starts() == 1);
  CHECK(wifi.ap_stops() == 0);
  CHECK(wifi.local_mode());
}

static void onboarding_and_offline_never_stop_ap() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  wifi.tick(WifiSta::Down, 0);
  wifi.tick(WifiSta::Down, 30000);
  wifi.set_onboarding(true);
  wifi.tick(WifiSta::Up, 40000);
  wifi.tick(WifiSta::Up, 43000);
  CHECK(wifi.confirmed());
  CHECK(!wifi.tick(WifiSta::Up, 43000 + 120000).stop_ap);
  CHECK(wifi.ap_on());
  wifi.set_onboarding(false);
  CHECK(wifi.tick(WifiSta::Up, 43000 + 120000).stop_ap);
  wifi.tick(WifiSta::Down, 43000 + 121000);
  CHECK(!wifi.tick(WifiSta::Down, 43000 + 151000).stop_ap);
  CHECK(wifi.ap_on());
  CHECK(wifi.ap_stops() == 1);
}

static void retries_are_bounded_and_do_not_stop_ap() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  uint32_t reconnects = 0;
  uint32_t stops = 0;
  for (uint32_t t = 0; t <= 30000; t += 10) {
    WifiCommand cmd = wifi.tick(WifiSta::Down, t);
    if (cmd.reconnect_sta) ++reconnects;
    if (cmd.stop_ap) ++stops;
  }
  CHECK(reconnects == wifi.reconnects());
  CHECK(reconnects >= 2);
  CHECK(reconnects <= 8);
  CHECK(stops == 0);
  CHECK(wifi.ap_starts() == 1);
}

static void millis_rollover_still_falls_back() {
  WifiFallback wifi;
  wifi.set_credentials(true);
  const uint32_t start = 0xFFFFFFF0u;
  CHECK(!wifi.tick(WifiSta::Down, start).start_ap);
  CHECK(!wifi.tick(WifiSta::Down, start + 29999u).start_ap);
  CHECK(wifi.tick(WifiSta::Down, start + 30000u).start_ap);
  CHECK(wifi.local_mode());
}

int main() {
  boot_offline_reaches_local_at_30s();
  boot_without_credentials_is_immediate();
  established_loss_waits_30s_from_the_drop();
  recovery_keeps_ap_for_120s_then_stops();
  flapping_does_not_postpone_fallback_or_churn_ap();
  onboarding_and_offline_never_stop_ap();
  retries_are_bounded_and_do_not_stop_ap();
  millis_rollover_still_falls_back();
  if (g_failures) {
    fprintf(stderr, "%d wifi fallback checks failed\n", g_failures);
    return 1;
  }
  printf("wifi fallback checks passed\n");
  return 0;
}
