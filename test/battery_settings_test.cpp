#include "battery_settings.h"

#include <stdio.h>
#include <string.h>

using namespace powercell;

static int g_failures = 0;

#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
  } \
} while (0)

static void auth_classifies_real_bearer() {
  CHECK(classify_lan_authorization(nullptr, LanKind::Lan) == SettingsAuth::Missing);
  CHECK(classify_lan_authorization("API-KEY secret", LanKind::None) == SettingsAuth::Malformed);
  CHECK(classify_lan_authorization("Bearer ota-token", LanKind::None) == SettingsAuth::Rejected);
  CHECK(classify_lan_authorization("Bearer session", LanKind::Session) == SettingsAuth::SessionOnly);
  CHECK(classify_lan_authorization("Bearer lan", LanKind::Lan) == SettingsAuth::Ok);
  BatterySettings store;
  SettingsDecision denied = store.set(SettingsAuth::SessionOnly, 1, true, 30, true, 18, 0, true);
  CHECK(!denied.applied);
  CHECK(denied.reason == SettingsReason::SessionNotSufficient);
  CHECK(store.state().low_percent == 25);
  CHECK(!store.set(SettingsAuth::Rejected, 1, true, 30, true, 18, 0, true).applied);
}

static void apply_then_readback_matches_request() {
  BatterySettings store;
  SettingsDecision applied = store.set(SettingsAuth::Ok, 1, true, 30, true, 18, 0, true);
  CHECK(applied.applied);
  CHECK(applied.durable_commit);
  CHECK(!applied.pending);
  CHECK(applied.reason == SettingsReason::None);
  CHECK(applied.readback.low_percent == 30);
  CHECK(applied.readback.warning_percent == 18);
  CHECK(applied.readback.inhibit_percent == 10);
  CHECK(applied.readback.restart_percent == 15);
  CHECK(applied.readback.applied_revision == 1);
  CHECK(!applied.readback.physical_shutdown_verified);
  SettingsDecision again = store.readback();
  CHECK(!again.applied);
  CHECK(again.readback.low_percent == 30);
  CHECK(again.readback.applied_revision == 1);
}

static void invalid_ranges_do_not_apply() {
  BatterySettings store;
  CHECK(store.set(SettingsAuth::Ok, 2, true, 30, true, 18, 0, true).reason ==
        SettingsReason::UnsupportedSchema);
  CHECK(store.set(SettingsAuth::Ok, 1, false, 30, true, 18, 0, true).reason ==
        SettingsReason::NotInteger);
  CHECK(store.set(SettingsAuth::Ok, 1, true, 30, true, 10, 0, true).reason ==
        SettingsReason::WarningNotAboveCutoff);
  CHECK(store.set(SettingsAuth::Ok, 1, true, 30, true, 5, 0, true).reason ==
        SettingsReason::WarningNotAboveCutoff);
  CHECK(store.set(SettingsAuth::Ok, 1, true, 90, true, 81, 0, true).reason ==
        SettingsReason::OutOfRange);
  CHECK(store.set(SettingsAuth::Ok, 1, true, 18, true, 18, 0, true).reason ==
        SettingsReason::LowNotAboveWarning);
  CHECK(store.set(SettingsAuth::Ok, 1, true, 12, true, 20, 0, true).reason ==
        SettingsReason::LowNotAboveWarning);
  CHECK(store.state().applied_revision == 0);
  CHECK(store.state().low_percent == 25);
}

static void stale_and_persist_failure_leave_canonical_state() {
  BatterySettings store;
  CHECK(store.set(SettingsAuth::Ok, 1, true, 30, true, 18, 0, true).applied);
  SettingsDecision stale = store.set(SettingsAuth::Ok, 1, true, 40, true, 20, 0, true);
  CHECK(!stale.applied);
  CHECK(!stale.durable_commit);
  CHECK(!stale.pending);
  CHECK(stale.reason == SettingsReason::StaleRevision);
  CHECK(stale.readback.low_percent == 30);
  CHECK(stale.readback.applied_revision == 1);
  SettingsDecision failed = store.set(SettingsAuth::Ok, 1, true, 40, true, 20, 1, false);
  CHECK(!failed.applied);
  CHECK(failed.reason == SettingsReason::PersistFailed);
  CHECK(failed.readback.low_percent == 30);
  CHECK(failed.readback.warning_percent == 18);
  CHECK(store.state().applied_revision == 1);
}

static void reboot_restores_only_saved_revision() {
  BatterySettings first;
  CHECK(first.set(SettingsAuth::Ok, 1, true, 28, true, 16, 0, true).applied);
  BatterySettings rebooted;
  rebooted.load(first.state().low_percent, first.state().warning_percent,
                first.state().applied_revision);
  CHECK(rebooted.readback().readback.low_percent == 28);
  CHECK(rebooted.readback().readback.warning_percent == 16);
  CHECK(rebooted.readback().readback.applied_revision == 1);
  CHECK(!rebooted.set(SettingsAuth::Ok, 1, true, 40, true, 20, 0, true).applied);
  CHECK(rebooted.state().low_percent == 28);
  BatterySettings lost;
  CHECK(!lost.set(SettingsAuth::Ok, 1, true, 28, true, 16, 0, false).applied);
  CHECK(lost.state().applied_revision == 0);
  CHECK(lost.state().low_percent == 25);
}

int main() {
  auth_classifies_real_bearer();
  apply_then_readback_matches_request();
  invalid_ranges_do_not_apply();
  stale_and_persist_failure_leave_canonical_state();
  reboot_restores_only_saved_revision();
  if (g_failures) {
    fprintf(stderr, "%d battery settings checks failed\n", g_failures);
    return 1;
  }
  printf("battery settings checks passed\n");
  return 0;
}
