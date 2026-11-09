#include "manual_limits.h"
#include <string.h>
#include "ac_output_policy.h"
#include "ac_output_settings.h"
#include "battery_settings.h"
#include "factory_rating.h"
#include "hw_pzem.h"
#include "power_guard.h"
#include "prefs_safe.h"
#include <Preferences.h>

namespace {
powercell::SystemRating g_rating(POWERCELL_FACTORY_RATED_ACTIVE_W, POWERCELL_FACTORY_VARIANT);
powercell::AcOutputSettings g_ac;
powercell::BatterySettings g_bat;
bool g_ready = false;

powercell::SettingsAuth authOf(int auth) {
  if (auth == 2) return powercell::SettingsAuth::Ok;
  if (auth == 1) return powercell::SettingsAuth::SessionOnly;
  return powercell::SettingsAuth::Missing;
}

void loadStore() {
  Preferences p;
  if (prefsOpenRO(p, "system_cfg")) {
    if (p.isKey("rated")) g_rating.observeStore(true, p.getInt("rated", 0));
    p.end();
  }
  Preferences ac;
  if (prefsOpenRO(ac, "ac_out")) {
    g_ac.load(ac.getInt("ceiling", 0), ac.getUChar("mode", 0), ac.getUInt("rev", 0));
    ac.end();
  }
  Preferences b;
  if (prefsOpenRO(b, "bat_set")) {
    g_bat.load(b.getInt("low", 25), b.getInt("warn", 15), b.getUInt("rev", 0));
    b.end();
    powerguard::setAlertThresholds(g_bat.state().low_percent, g_bat.state().warning_percent);
  }
}

bool saveAc() {
  Preferences p;
  if (!p.begin("ac_out", false)) return false;
  p.putInt("ceiling", g_ac.state().ceiling_w);
  p.putUChar("mode", g_ac.state().mode == powercell::AcModeSetting::WarnAndShutdown ? 1 : 0);
  p.putUInt("rev", g_ac.state().applied_revision);
  p.end();
  return true;
}
bool saveBat() {
  Preferences p;
  if (!p.begin("bat_set", false)) return false;
  p.putInt("low", g_bat.state().low_percent);
  p.putInt("warn", g_bat.state().warning_percent);
  p.putUInt("rev", g_bat.state().applied_revision);
  p.end();
  return true;
}

void applyCeiling() {
  const auto& s = g_ac.state();
  powercell::acOutputPolicy().setUsable(g_rating.armed() ? g_rating.usableW() : 0);
  powercell::acOutputPolicy().setCeiling(s.ceiling_w, s.mode == powercell::AcModeSetting::WarnAndShutdown
                                                            ? powercell::AcMode::WarnAndShutdown
                                                            : powercell::AcMode::WarnOnly);
}
}  // namespace

void manualLimitsBegin() {
  if (g_ready) return;
  g_ready = true;
  loadStore();
  applyCeiling();
}

void manualLimitsService(uint32_t now) {
#if POWERCELL_HW_V011
  manualLimitsBegin();
  float volts = 0, amps = 0, watts = 0;
  uint32_t wh = 0, age = 0;
  powercell::AcWatts sample;
  sample.coverage_verified = false;
  if (hwPzemSample(&volts, &amps, &watts, &wh, &age)) {
    sample.valid = true;
    sample.watts = watts;
    sample.receivedAtMs = now - age;
  }
  applyCeiling();
  powercell::acOutputPolicy().update(sample, now);
#else
  (void)now;
#endif
}

void manualSetupRating(JsonDocument& doc) {
  manualLimitsBegin();
  const auto& st = g_rating.state();
  doc["rated_kva"] = nullptr;
  if (st.overload_configured && st.factory_rated_w == powercell::SystemRating::kRated5kW) doc["powercell_model"] = "PC5K";
  else doc["powercell_model"] = nullptr;
  if (st.overload_configured) doc["usable_kw"] = st.usable_active_w / 1000.0;
  else doc["usable_kw"] = nullptr;
}

void manualStatusRating(JsonObject obj) {
  manualLimitsBegin();
  const auto& st = g_rating.state();
  obj["factory_variant"] = st.variant;
  obj["overload_configured"] = st.overload_configured;
  obj["rating_writable"] = false;
  obj["total_output_coverage_verified"] = false;
  if (st.overload_configured) {
    obj["rated_active_w"] = st.factory_rated_w;
    obj["usable_active_w"] = st.usable_active_w;
  } else {
    obj["rated_active_w"] = nullptr;
    obj["usable_active_w"] = nullptr;
  }
}

int manualSystemRating(JsonDocument& out, int auth, bool post) {
  manualLimitsBegin();
  const auto& st = g_rating.state();
  out["schema"] = 1;
  out["result"] = post ? "rejected" : "readback";
  out["reason"] = post ? "factory_provisioned" : g_rating.reason();
  out["applied"] = false;
  out["durable_commit"] = false;
  out["writable"] = false;
  out["authority"] = "factory_image";
  out["overload_configured"] = st.overload_configured;
  out["physical_shutdown_verified"] = false;
  out["factory_variant"] = st.variant;
  JsonObject rb = out["readback"].to<JsonObject>();
  rb["rated_active_w"] = st.factory_rated_w;
  rb["factory_rated_active_w"] = st.factory_rated_w;
  if (st.overload_configured) rb["usable_active_w"] = st.usable_active_w;
  else rb["usable_active_w"] = nullptr;
  if (st.store_present) rb["stored_rated_active_w"] = st.stored_rated_w;
  else rb["stored_rated_active_w"] = nullptr;
  rb["rating_match"] = st.overload_configured;
  if (post) return 403;
  if (auth != 2) {
    out["reason"] = auth == 1 ? "session_not_sufficient" : "unauthorized";
    return auth == 1 ? 403 : 401;
  }
  return 200;
}

int manualAcSettings(JsonDocument& doc, JsonDocument& out, int auth, bool post) {
  manualLimitsBegin();
  if (!post) {
    const auto d = g_ac.readback();
    out["schema"] = 1;
    out["result"] = "readback";
    out["reason"] = "none";
    out["applied"] = false;
    out["durable_commit"] = false;
    out["physical_shutdown_verified"] = false;
    JsonObject rb = out["readback"].to<JsonObject>();
    rb["schema"] = 1;
    rb["ceiling_w"] = d.readback.ceiling_w;
    rb["mode"] = d.readback.mode == powercell::AcModeSetting::WarnAndShutdown ? "warn_and_shutdown" : "warn_only";
    rb["applied_revision"] = d.readback.applied_revision;
    if (auth != 2) return auth == 1 ? 403 : 401;
    return 200;
  }
  const char* mode = doc["mode"].is<const char*>() ? doc["mode"].as<const char*>() : "";
  int mode_n = !strcmp(mode, "warn_only") ? 0 : !strcmp(mode, "warn_and_shutdown") ? 1 : -1;
  bool ceiling_int = doc["ceiling_w"].is<int>() || doc["ceiling_w"].is<long>();
  int ceiling = ceiling_int ? doc["ceiling_w"].as<int>() : 0;
  uint32_t base = doc["base_revision"].is<long>() ? (uint32_t)doc["base_revision"].as<long>() : 0;
  bool can_store = false;
  if (authOf(auth) == powercell::SettingsAuth::Ok) {
    Preferences probe;
    can_store = probe.begin("ac_out", false);
    if (can_store) probe.end();
  }
  auto decision = g_ac.set(authOf(auth), doc["schema"] | 1, ceiling_int, ceiling, mode_n >= 0, mode_n,
                           g_rating.armed() ? g_rating.usableW() : 0, base, can_store);
  if (decision.applied && saveAc()) applyCeiling();
  out["schema"] = 1;
  out["result"] = decision.applied ? "applied" : "rejected";
  out["reason"] = powercell::settings_reason_name(decision.reason);
  out["applied"] = decision.applied;
  out["durable_commit"] = decision.durable_commit;
  out["physical_shutdown_verified"] = false;
  JsonObject rb = out["readback"].to<JsonObject>();
  rb["schema"] = 1;
  rb["ceiling_w"] = decision.readback.ceiling_w;
  rb["mode"] = decision.readback.mode == powercell::AcModeSetting::WarnAndShutdown ? "warn_and_shutdown" : "warn_only";
  rb["applied_revision"] = decision.readback.applied_revision;
  if (auth != 2) return auth == 1 ? 403 : 401;
  return decision.applied ? 200 : 400;
}

int manualBatterySettings(JsonDocument& doc, JsonDocument& out, int auth, bool post) {
  manualLimitsBegin();
  auto fill = [&](const powercell::SettingsDecision& d, bool applied) {
    out["schema"] = 1;
    out["result"] = applied ? "applied" : (post ? "rejected" : "readback");
    out["reason"] = powercell::settings_reason_name(d.reason);
    out["applied"] = d.applied;
    out["durable_commit"] = d.durable_commit;
    out["pending"] = false;
    out["physical_shutdown_verified"] = false;
    JsonObject rb = out["readback"].to<JsonObject>();
    rb["schema"] = 1;
    rb["low_percent"] = d.readback.low_percent;
    rb["warning_percent"] = d.readback.warning_percent;
    rb["inhibit_percent"] = d.readback.inhibit_percent;
    rb["restart_percent"] = d.readback.restart_percent;
    rb["applied_revision"] = d.readback.applied_revision;
    rb["durable"] = d.readback.durable;
  };
  if (!post) {
    fill(g_bat.readback(), false);
    if (auth != 2) return auth == 1 ? 403 : 401;
    return 200;
  }
  bool low_int = doc["low_percent"].is<int>() || doc["low_percent"].is<long>();
  bool warn_int = doc["warning_percent"].is<int>() || doc["warning_percent"].is<long>();
  int low = low_int ? doc["low_percent"].as<int>() : 0;
  int warn = warn_int ? doc["warning_percent"].as<int>() : 0;
  uint32_t base = doc["base_revision"].is<long>() ? (uint32_t)doc["base_revision"].as<long>() : 0;
  bool can_store = false;
  if (authOf(auth) == powercell::SettingsAuth::Ok) {
    Preferences probe;
    can_store = probe.begin("bat_set", false);
    if (can_store) probe.end();
  }
  auto decision = g_bat.set(authOf(auth), doc["schema"] | 1, low_int, low, warn_int, warn, base, can_store);
  if (decision.applied && saveBat()) {
    powerguard::setAlertThresholds(decision.readback.low_percent, decision.readback.warning_percent);
  }
  fill(decision, decision.applied);
  if (auth != 2) return auth == 1 ? 403 : 401;
  return decision.applied ? 200 : 400;
}
