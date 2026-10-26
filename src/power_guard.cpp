#include "prefs_safe.h"
#include "power_guard.h"
#include <Preferences.h>
#include <WiFi.h>
#include <map>
#include "battery.h"
#include "bms.h"
#include "config.h"
#include "gateway.h"
#include "log.h"
#include "events_core.h"
#include "soc_guard.h"
#include "socket_cfg.h"
#include "sockets/sockets.h"
#include "stm32_link.h"

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

bool espLatched() { return g_guard.latched(); }
bool stmReserve() { return g_stmRsv; }
bool latched() { return g_guard.latched() || g_stmRsv; }
bool lowPower() { return g_low; }
bool refuseOn(const char *origin) {
  if (g_stmRsv) return true;  // only the STM32 can release its reserve latch
  return g_guard.latched() && strcmp(origin ? origin : "", "soc_restore") != 0;
}

void setPriority(const String &id, bool essential) { socketcfg::setPriority(id, essential); }  // console PRIO fallback

void begin() {
  Preferences p;
  bool l = false;
  if (prefsOpenRO(p, NS)) { l = p.getBool("latched", false); g_wasOn = p.getBool("wasOn", false); p.end(); }
  g_guard.restore(l);
  g_restorePending = !l && g_wasOn;  // ESP released before a reboot, STM32 latch may still hold
  if (l) { LOGW("PWR", "LATCH_RESTORED", "battery critical latch from NVS; output held off"); setLowPower(true); }
}

void service() {
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
  in.stmCharging = stm && (s.chr == pc::ChargeState::Bulk || s.chr == pc::ChargeState::Absorb || s.chr == pc::ChargeState::Float);
  Change c = g_guard.update(in, now);
  if (c == Change::Latched) {
    g_wasOn = gateway::state(); g_offDone = false; g_lastOffTry = 0;
    saveLatch();
    LOGE("PWR", "CRITICAL_LATCH", "soc=%.0f%% source=%s: inverter output off", g_soc, b.source);
  } else if (c == Change::Released) {
    saveLatch();
    LOGW("PWR", "CRITICAL_RELEASE", "soc=%.0f%% charging held 60 s; restore=%d", g_soc, (int)g_wasOn);
    g_restorePending = g_wasOn;  // applied once the STM32 reserve latch is clear too
  }
  if (g_restorePending && !g_guard.latched() && !g_stmRsv) {
    g_restorePending = false;
    gateway::Result r = gateway::set(true, "soc_restore");
    LOGI("PWR", "RESTORE", "%s", gateway::resultName(r));
    g_wasOn = false; saveLatch();
  }
  if (g_guard.latched() && !g_offDone && (!g_lastOffTry || now - g_lastOffTry >= 5000)) {
    g_lastOffTry = now;  // framed: set_mode OFF; legacy: PWR_KEY released. Retried every 5 s.
    gateway::Result r = gateway::set(false, "battery_critical");
    g_offDone = r == gateway::Result::Ok && stm32link::online();
    if (!g_offDone) LOGW("PWR", "OFF_RETRY", "%s stm32_online=%d", gateway::resultName(r), (int)stm32link::online());
  }
  setLowPower(latched());
  if (now - g_lastShed >= 2000) { g_lastShed = now; shedPass(); }
}

void toJson(JsonObject o) {
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
}

String debugJson() { JsonDocument d; toJson(d.to<JsonObject>()); String s; serializeJson(d, s); return s; }
}  // namespace powerguard
