#include "prefs_safe.h"
#include "power_guard.h"
#include <Preferences.h>
#include <WiFi.h>
#include <map>
#include "battery.h"
#include "battery_alert_policy.h"
#include "bms.h"
#include "config.h"
#include "ac_output_policy.h"
#include "gateway.h"
#include "dsp011_pzem.h"
#include "manual_limits.h"
#include "hw_output.h"
#include "log.h"
#include "events_core.h"
#include "local_auth.h"
#include "soc_guard.h"
#include "socket_cfg.h"
#include "sockets/sockets.h"
#include "stm32_link.h"
#include "dsp_thermal.h"

namespace powerguard {
namespace {
using namespace pc::soc;
CriticalGuard g_guard;
bool g_wasOn = false;          // output state before the ESP latch (restored on release)
bool g_offDone = false;        // shutdown command accepted since latch/boot
uint32_t g_lastOffTry = 0, g_last = 0, g_lastShed = 0;
bool g_stmRsv = false, g_low = false, g_restorePending = false;
float g_soc = 0; bool g_socValid = false;
bool g_gridDown = true;   // STM32 grid < 160 V (restore 170 V); unknown/stale = down
bool g_overload = false;  // inverter overload (STM32 bit16 or iout >= 19 A, clear 17 A)
const char *NS = "pwr_guard";
float g_alertLow = pc::BatteryAlertPolicy::kLow, g_alertWarn = pc::BatteryAlertPolicy::kCritical;
#if POWERCELL_DSP011
powercell::BatteryAlertPolicy g_cutoff;
bool g_cutoffOff = false;
const char *kCutoffNs = "battery_guard";

void saveCutoff() {
  Preferences p;
  if (!p.begin(kCutoffNs, false)) { g_cutoff.ackLatchSave(false); return; }
  p.putUChar("latch", g_cutoff.outputLatched() ? 1 : 0);
  p.end();
  g_cutoff.ackLatchSave(true);
}
powercell::thermal::OverTempGuard g_ot;
bool g_otOff = false;
uint32_t g_otLastTry = 0;

void saveOverTemp() {
  Preferences p;
  if (!p.begin(kCutoffNs, false)) return;  // dirty stays set: retried next pass
  p.putUChar("ot_latch", g_ot.latched() ? 1 : 0);
  p.end();
  g_ot.ackSaved();
}
bool thermalNow(float *c) {
  DspThermalReading t;
  const bool ok = dspThermalRead(&t);
  *c = t.temp_c;
  return ok;
}
void holdAcOff() {
  if (!powercell::battery_ac_output_auto_off(powercell::HardwareProfile::V011)) return;
  if (gateway::state() || !g_cutoffOff) {
    gateway::Result r = gateway::set(false, "battery_critical");
    g_cutoffOff = r == gateway::Result::Ok;
  }
}
#endif

// Estimated ESP-side draw at 3.3 V (datasheet averages; excludes STM32, BMS, inverter board).
constexpr float kPackAh = 230, kPackV = 51.2f, kIdleW = 6.2f;
constexpr float kMaNormal = 45 + 11 + 16;  // Wi-Fi min-modem sleep + SX1276 RX + ~2 LEDs lit
constexpr float kMaLow = 20 + 0.5f + 0.4f; // Wi-Fi max-modem sleep + LoRa duty ~8 % RX + red 100 ms / 4 s

void saveLatch() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putBool("latched", g_guard.latched()); p.putBool("wasOn", g_wasOn);
  p.end();
}
void setLowPower(bool on) {
  if (on == g_low) return;
  g_low = on;
  // Modem sleep is the reserve-mode default. WiFi.setSleep() keeps the type across reconnects
  // and mode changes; the WS stays connected (15 s ping, DTIM wake).
  WiFi.setSleep(on ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
  socketsSetLowPower(on);
  LOGW("PWR", on ? "LOW_POWER_ON" : "LOW_POWER_OFF", "est_esp_draw=%.0f mA @3.3V", on ? kMaLow : kMaNormal);
}

// Per-socket rules (safety, SoC shed, overload, cloud override, schedules) live in socket_cfg.
void shedPass() { socketcfg::pass(g_socValid, g_soc, g_gridDown, latched(), g_overload); }
}  // namespace

bool espLatched() {
#if POWERCELL_DSP011
  return g_cutoff.outputLatched() || !g_cutoff.outputOnAllowed();
#else
  return g_guard.latched();
#endif
}
bool stmReserve() { return g_stmRsv; }
bool latched() {
#if POWERCELL_DSP011
  return !g_cutoff.outputOnAllowed();
#else
  return g_guard.latched() || g_stmRsv;
#endif
}
bool lowPower() { return g_low; }
// Saved Manual battery-low / shutdown-warning percents (manual_limits, /v1/battery-settings).
// All builds: they drive the /events battery_low|battery_critical notifications and the
// cloud battery_soc event. DSP: also the BatteryAlertPolicy warning. The 10 % cutoff and the
// 15 % restart are fixed. A pair that is out of range or misordered (bad NVS) uses 25/15.
void setAlertThresholds(float lowPercent, float warningPercent) {
  if (!pc::BatteryAlertPolicy::validThresholds(lowPercent, warningPercent)) {
    LOGW("PWR", "ALERT_THRESHOLDS", "saved low=%.0f warning=%.0f invalid; using %.0f/%.0f", lowPercent, warningPercent,
         pc::BatteryAlertPolicy::kLow, pc::BatteryAlertPolicy::kCritical);
    lowPercent = pc::BatteryAlertPolicy::kLow;
    warningPercent = pc::BatteryAlertPolicy::kCritical;
  }
  g_alertLow = lowPercent;
  g_alertWarn = warningPercent;
#if POWERCELL_DSP011
  g_cutoff.setThresholds(lowPercent, warningPercent);
#endif
}
float alertLowPercent() { return g_alertLow; }
float alertWarningPercent() { return g_alertWarn; }

bool overloadLatched() {
#if POWERCELL_DSP011
  return powercell::acOutputPolicy().shutdownLatched();
#else
  return false;
#endif
}

bool overTempLatched() {
#if POWERCELL_DSP011
  return g_ot.latched();
#else
  return false;
#endif
}
bool overTempBlocksOn() {
#if POWERCELL_DSP011
  float c = 0;
  const bool ok = thermalNow(&c);
  return g_ot.blocksOn(ok, c);
#else
  return false;
#endif
}
bool overTempRefuseOn(const char *origin) {
#if POWERCELL_DSP011
  float c = 0;
  const bool ok = thermalNow(&c);
  const bool was = g_ot.latched();
  if (!g_ot.acceptOn(ok, c)) {
    if (ok) LOGW("PWR", "OVER_TEMP_REFUSED", "on refused at %.1f C (origin=%s latched=%d)", c, origin ? origin : "?", (int)was);
    else LOGW("PWR", "OVER_TEMP_REFUSED", "on refused: no valid temperature (origin=%s latched=%d)", origin ? origin : "?", (int)was);
    return true;
  }
  if (was) {
    saveOverTemp();
    g_otOff = false;
    LOGW("PWR", "OVER_TEMP_CLEARED", "user on at %.1f C (< %.0f C, origin=%s)", c, powercell::thermal::kOnAllowedBelowC, origin ? origin : "?");
  }
  return false;
#else
  (void)origin;
  return false;
#endif
}

bool refuseOn(const char *origin) {
#if POWERCELL_DSP011
  (void)origin;
  return !g_cutoff.outputOnAllowed();
#else
  if (!strcmp(origin ? origin : "", "soc_restore")) return false;  // guard's own restore
  // 2.5.5: battery percentage gate (BMS SoC > 10 %), 48.0 V fallback without the BMS.
  OnGateIn in; in.espLatched = g_guard.latched(); in.stmReserve = g_stmRsv;
  battery::Reading b = battery::read();
  in.bmsSocValid = b.valid && !strcmp(b.source, "bms");
  in.bmsSoc = b.soc;
  pc::Stm32Status s; uint32_t age = 0;
  if (stm32link::status(s, &age) && age < STM32_STALE_MS && s.has_bat) { in.vbatValid = true; in.vbat = s.bat_v; }
  OnGate g = onGate(in);
  if (g != OnGate::Allow) {
    LOGW("PWR", "ON_REFUSED", "%s soc=%u vbat=%.2f", g == OnGate::RefuseSoc ? (in.espLatched ? "latched, bms soc < 15%" : "bms soc <= 10%") : g == OnGate::RefuseVoltage ? "no bms, vbat < 48.0 V" : "no soc/voltage, latched", (unsigned)b.soc, in.vbat);
    return true;
  }
  if (g_guard.latched()) {  // manual ON at >= 15 % BMS SoC (or the 48 V fallback) clears the ESP latch; it re-latches at <= 10 %
    g_guard.restore(false); g_restorePending = false; saveLatch();
    LOGW("PWR", "CRITICAL_OVERRIDE", "on at restart level (origin=%s): ESP latch cleared", origin ? origin : "?");
  }
  if (g_stmRsv) {  // 2.5.6: gate passed, ask the STM32 (XFER4) to clear its reserve latch first
    char rep[48] = {};
    auto r = stm32link::textQuery("RSV CLR", "RSV:", rep, sizeof rep, 500);
    if (r == stm32link::Result::Ok) {
      pc::RsvReply rr = pc::parseRsvReply(rep);
      if (rr != pc::RsvReply::Ok) {
        LOGW("PWR", "RSV_CLR", "refused by STM32: %s (%s); on refused", rep, pc::rsvReplyName(rr));
        return true;
      }
      LOGI("PWR", "RSV_CLR", "STM32 reserve latch cleared (%s)", rep);
      g_stmRsv = false;
    } else {
      LOGW("PWR", "RSV_CLR", "no reply (%s, pre-XFER4 STM32?); sending on anyway", stm32link::resultName(r));
    }
  }
  return false;
#endif
}

void setPriority(const String &id, bool essential) { socketcfg::setPriority(id, essential); }  // console PRIO fallback

void begin() {
  manualLimitsBegin();  // saved Manual battery-low / warning percents, all builds (was DSP first service only)
#if POWERCELL_DSP011
  g_cutoff = powercell::BatteryAlertPolicy(millis());
  g_cutoff.setThresholds(g_alertLow, g_alertWarn);
  // A read-only open of a namespace that was never written returns NOT_FOUND.
  // That is "no latch saved", not a failed read. Treating it as unreadable
  // made outputOnAllowed() false on every boot, so cloud ON was rejected with
  // battery_critical_latched while the pack was full. The 60s clear was also
  // not saved, so the next reboot (the next websocket session) did it again.
  bool opened = false, not_found = false, has_key = false, held = false;
  nvs_handle_t h;
  const esp_err_t err = nvs_open(kCutoffNs, NVS_READONLY, &h);
  if (err == ESP_OK) {
    uint8_t v = 0;
    const esp_err_t kr = nvs_get_u8(h, "latch", &v);
    nvs_close(h);
    if (kr == ESP_OK) {
      opened = true;
      has_key = true;
      held = v != 0;
    } else if (kr == ESP_ERR_NVS_NOT_FOUND) {
      opened = true;
    }
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    not_found = true;
  }
  g_cutoff.applyLatchStore(powercell::BatteryAlertPolicy::classifyLatchStore(opened, not_found, has_key, held));
  if (!g_cutoff.outputOnAllowed()) holdAcOff();
  {  // over-temperature latch, same namespace. Missing key = not latched.
    uint8_t ot = 0;
    if (nvs_open(kCutoffNs, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "ot_latch", &ot); nvs_close(h); }
    g_ot.restore(ot != 0);
    if (g_ot.latched()) {
      LOGW("PWR", "OVER_TEMP_RESTORED", "over-temperature latch from NVS; output held off until < %.0f C and a user on",
           powercell::thermal::kOnAllowedBelowC);
      gateway::Result r = gateway::set(false, "over_temperature");
      g_otOff = r == gateway::Result::Ok;
    }
  }
  return;
#else
  Preferences p;
  bool l = false;
  if (prefsOpenRO(p, NS)) { l = p.getBool("latched", false); g_wasOn = p.getBool("wasOn", false); p.end(); }
  g_guard.restore(l);
  g_restorePending = !l && g_wasOn;  // ESP released before a reboot, STM32 latch may still hold
  if (l) { LOGW("PWR", "LATCH_RESTORED", "battery critical latch from NVS; output held off"); setLowPower(true); }
#endif
}

void service() {
#if POWERCELL_DSP011
  uint32_t now = millis();
  if (now - g_last < 1000) return;
  g_last = now;
  pc::jk::Data d;
  uint32_t age = 0;
  powercell::BatteryReading reading = {};
  if (bms::sample(d, &age, powercell::BatteryAlertPolicy::FreshnessMs)) {
    reading.valid = true;
    reading.percent = d.soc;
    reading.voltage = d.packMv / 1000.0f;
    reading.receivedAtMs = now - age;
    reading.charging = d.status == 1;  // recorded; a full or idle pack can still release
    reading.bms_own = true;
  }
  g_cutoff.update(reading, now);
  if (g_cutoff.latchDirty()) saveCutoff();
  if (!g_cutoff.outputOnAllowed()) holdAcOff();
  else g_cutoffOff = false;  // eligible again; do not turn the output on
  {  // over-temperature cutoff: >= 80 C latches, GPIO15 off, no auto-on
    float c = 0;
    const bool ok = thermalNow(&c);
    if (g_ot.update(ok, c)) {
      g_otOff = false; g_otLastTry = 0;
      LOGE("PWR", "OVER_TEMP_CUTOFF", "system temperature %.1f C >= %.0f C: inverter output off (latched)", c, powercell::thermal::kCutoffC);
    }
    if (g_ot.dirty()) saveOverTemp();
    if (g_ot.latched() && (gateway::state() || (!g_otOff && (!g_otLastTry || now - g_otLastTry >= 5000)))) {
      g_otLastTry = now;
      gateway::Result r = gateway::set(false, "over_temperature");
      g_otOff = r == gateway::Result::Ok;
    }
  }
  manualLimitsService(now);
  if (powercell::acOutputPolicy().shutdownLatched()) {
    const bool edge = powercell::acOutputPolicy().takeShutdownEdge();
    if (gateway::state() || edge) gateway::set(false, "ac_overload");
  }
  return;
#else
  uint32_t now = millis();
  if (now - g_last < 1000) return;
  g_last = now;
  battery::Reading b = battery::read();
  // BMS SoC, STM32 voltage fallback. The IO34 ADC last resort is not trusted to shut the output.
  g_socValid = b.valid && strcmp(b.source, "adc") != 0;
  g_soc = b.soc;
  pc::Stm32Status s; uint32_t age = 0;
  bool stm = stm32link::status(s, &age) && age < STM32_STALE_MS;
  if (stm) {
    bool r = s.has_rsv && s.rsv_latched;
    if (r != g_stmRsv) LOGW("PWR", "STM32_RSV", "reserve latch %s", r ? "active" : "released");
    g_stmRsv = r;
    bool ov = s.has_ext && (s.fault & pc::ev::kStatusOverloadWarn);
    if (!s.has_ext && s.has_iout) ov = s.iout >= (g_overload ? pc::ev::kOverloadClearAmps : pc::ev::kOverloadAmps);
    g_overload = ov;
    g_gridDown = !s.has_grid || s.grid_v < (g_gridDown ? pc::ev::kGridRestoreV : pc::ev::kGridMinV);
  } else { g_overload = false; g_gridDown = true; }
  GuardIn in;
  in.socValid = g_socValid; in.soc = g_soc;
  pc::jk::Data d;
  if ((in.bmsOnline = bms::read(d))) in.bmsCurrentA = d.currentMa / 1000.0f;
  in.bmsSocValid = in.bmsOnline && b.valid && strcmp(b.source, "bms") == 0;
  in.stmCharging = stm && (s.chr == pc::ChargeState::Bulk || s.chr == pc::ChargeState::Absorb || s.chr == pc::ChargeState::Float);
  Change c = g_guard.update(in, now);
  if (c == Change::Latched) {
    g_wasOn = gateway::state(); g_offDone = false; g_lastOffTry = 0;
    saveLatch();
    LOGE("PWR", "CRITICAL_LATCH", "soc=%.0f%% source=%s: inverter output off", g_soc, b.source);
  } else if (c == Change::Released) {
    saveLatch();
    LOGW("PWR", "CRITICAL_RELEASE", "soc=%.0f%% source=%s held 60 s (charging or healthy BMS); restore=%d", g_soc, b.source, (int)g_wasOn);
    g_restorePending = g_wasOn;  // applied once the STM32 reserve latch is clear too
  }
  if (g_restorePending && !g_guard.latched() && !g_stmRsv) {
#if POWERCELL_DSP011
    g_restorePending = false;  // recovery does not turn the AC output on
#else
    g_restorePending = false;
    gateway::Result r = gateway::set(true, "soc_restore");
    LOGI("PWR", "RESTORE", "%s", gateway::resultName(r));
    g_wasOn = false; saveLatch();
#endif
  }
  if (g_guard.latched() && !g_offDone && (!g_lastOffTry || now - g_lastOffTry >= 5000)) {
    g_lastOffTry = now;  // framed: set_mode OFF; legacy: PWR_KEY released. Retried every 5 s.
    gateway::Result r = gateway::set(false, "battery_critical");
    g_offDone = r == gateway::Result::Ok && stm32link::online();
    if (!g_offDone) LOGW("PWR", "OFF_RETRY", "%s stm32_online=%d", gateway::resultName(r), (int)stm32link::online());
  }
  setLowPower(latched());
  if (now - g_lastShed >= 2000) { g_lastShed = now; shedPass(); }
#endif
}

void toJson(JsonObject o) {
#if POWERCELL_DSP011
  o["critical_latched"] = g_cutoff.outputLatched();
  o["boot_hold"] = !g_cutoff.outputOnAllowed() && !g_cutoff.outputLatched();
  o["output_on_allowed"] = g_cutoff.outputOnAllowed();
  o["physical_shutdown_verified"] = powercell::kPhysicalShutdownVerified;
  o["stm32_reserve_latched"] = false;
  o["charging_stopped"] = powercell::battery_cutoff_stops_charging();
  o["over_temp_cutoff"] = g_ot.latched();
  o["over_temp_blocks_on"] = overTempBlocksOn();
  o["over_temp_cutoff_c"] = powercell::thermal::kCutoffC;
  o["over_temp_on_below_c"] = powercell::thermal::kOnAllowedBelowC;
  return;
#else
  o["critical_latched"] = g_guard.latched();
  o["stm32_reserve_latched"] = g_stmRsv;
  o["low_power"] = g_low;
  if (g_socValid) o["soc"] = g_soc; else o["soc"] = nullptr;
  o["est_esp_draw_ma"] = g_low ? kMaLow : kMaNormal;  // at 3.3 V
  o["est_esp_draw_w"] = (g_low ? kMaLow : kMaNormal) * 3.3f / 1000.0f;
  // Bench (Caleb): whole-unit idle about 6.2 W, 230 Ah pack (51.2 V nominal): 10 % lasts about 8 days.
  o["wifi_ps"] = g_low ? "max_modem" : "min_modem";
  if (g_socValid) o["est_runtime_h_at_idle"] = g_soc / 100.0f * kPackAh * kPackV / kIdleW; else o["est_runtime_h_at_idle"] = nullptr;
  o["priority_source"] = "socket_config";
  o["inverter_overload"] = g_overload;
  o["grid_down"] = g_gridDown;
  socketcfg::shedList(o["shed_sockets"].to<JsonArray>());
#endif
}

String debugJson() { JsonDocument d; toJson(d.to<JsonObject>()); String s; serializeJson(d, s); return s; }
}  // namespace powerguard
