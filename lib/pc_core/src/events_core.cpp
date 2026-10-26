#include "events_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "jk_bms.h"

namespace pc {
namespace ev {
const char *typeName(Type t) {
  switch (t) {
    case Type::Overload: return "overload"; case Type::Overcurrent: return "overcurrent";
    case Type::InverterFault: return "inverter_fault"; case Type::BmsFault: return "bms_fault";
    case Type::RelayFailed: return "relay_failed"; case Type::OverTemp: return "over_temp";
    case Type::BatteryFull: return "battery_full"; case Type::BatteryLow: return "battery_low";
    case Type::GridDown: return "grid_down";
    case Type::ApplianceLockout: return "appliance_lockout";
    case Type::BatteryCritical: return "battery_critical";
    case Type::OutputRestored: return "output_restored";
    case Type::ModeFallback: return "mode_fallback";
    case Type::LoadShed: return "load_shed";
  }
  return "unknown";
}
const char *sevName(Sev s) { return s == Sev::Critical ? "critical" : s == Sev::Warning ? "warning" : "info"; }

Engine::Cond *Engine::find(const char *id, bool create) {
  Cond *free = nullptr;
  for (auto &c : c_) {
    if (c.used && !strcmp(c.ev.id, id)) return &c;
    if (!c.used && !free) free = &c;
  }
  if (!create) return nullptr;
  if (!free) {  // reuse an inactive slot
    for (auto &c : c_) if (!c.active && c.raw == false) { free = &c; break; }
    if (!free) return nullptr;
  }
  *free = Cond();
  free->used = true;
  snprintf(free->ev.id, sizeof free->ev.id, "%s", id);
  return free;
}

bool Engine::isActive(const char *id) const {
  for (auto &c : c_) if (c.used && !strcmp(c.ev.id, id)) return c.active;
  return false;
}

void Engine::push(const Event &e) {
  if (outN_ == kOut) { outHead_ = (outHead_ + 1) % kOut; --outN_; ++dropped_; }
  out_[(outHead_ + outN_) % kOut] = e;
  ++outN_;
}

void Engine::cond(const Spec &s, bool raw, uint32_t now, uint32_t ts) {
  Cond *c = find(s.id, raw);
  if (!c) return;  // never seen and not raised: nothing to track
  auto fill = [&](Event &e) {
    e.type = s.type; e.severity = s.sev; e.hasValue = s.hasValue; e.value = s.value;
    snprintf(e.code, sizeof e.code, "%s", s.code ? s.code : "");
    snprintf(e.socket, sizeof e.socket, "%s", s.socket ? s.socket : "");
    e.source = s.source; e.permanent = s.permanent; e.releaseRequires = s.releaseRequires;
  };
  if (raw != c->raw) { c->raw = raw; c->since = now; }
  if (raw == c->active) {
    if (raw) {  // still active: escalate or de-escalate severity, keep value fresh
      bool sevChanged = c->ev.severity != s.sev || c->ev.permanent != s.permanent || strcmp(c->ev.code, s.code ? s.code : "");
      fill(c->ev);
      if (sevChanged) { c->ev.ts = ts; push(c->ev); }
    }
    return;
  }
  if (now - c->since < (raw ? s.onMs : s.offMs)) return;
  c->active = raw;
  if (raw) fill(c->ev);
  else if (s.hasValue) { c->ev.hasValue = true; c->ev.value = s.value; }  // clear carries the latest value
  c->ev.active = raw;
  c->ev.ts = ts;
  push(c->ev);
}

namespace {
struct BitMap { Type type; const char *code; };
const BitMap kStmBits[8] = {
  {Type::Overcurrent, "overcurrent"}, {Type::Overcurrent, "short_circuit"}, {Type::InverterFault, "dc_overvoltage"},
  {Type::BatteryLow, "dc_undervoltage"}, {Type::InverterFault, "output_overvoltage"}, {Type::OverTemp, "thermal"},
  {Type::InverterFault, "ntc_open"}, {Type::InverterFault, "no_feedback"},
};
struct BmsMap { Type type; Sev sev; };
BmsMap bmsMap(int bit) {
  switch (bit) {
    case 1: case 8: case 15: case 21: return {Type::OverTemp, Sev::Critical};
    case 6: case 7: case 13: case 14: return {Type::Overcurrent, Sev::Critical};
    case 11: case 12: return {Type::BatteryLow, Sev::Critical};
    case 4: case 5: case 16: case 17: return {Type::BmsFault, Sev::Critical};
    case 18: case 19: return {Type::BmsFault, Sev::Info};
    default: return {Type::BmsFault, Sev::Warning};
  }
}
// Threshold with hysteresis: rises at `on`, falls below `off`.
bool hyst(bool active, float v, float on, float off) { return active ? v >= off : v >= on; }
bool hystBelow(bool active, float v, float on, float off) { return active ? v < off : v < on; }
}  // namespace

void Engine::update(const Inputs &in, uint32_t now, uint32_t ts) {
  char id[32], code[24];
  // ---- STM32 ----------------------------------------------------------------
  // Offline STM32: hold every STM32 condition as it was (we cannot know).
  if (in.stmOnline) {
    const bool latched = in.fault & kStatusPermanent;
    for (int b = 0; b < 16; ++b) {
      uint32_t m = 1u << b;
      snprintf(id, sizeof id, "stm32_0x%02lX", (unsigned long)m);
      Type t = b < 8 ? kStmBits[b].type : Type::InverterFault;
      if (b < 8) snprintf(code, sizeof code, "0x%02lX_%s", (unsigned long)m, kStmBits[b].code);
      else snprintf(code, sizeof code, "0x%04lX", (unsigned long)m);
      bool perm = (m & kPermanentBits) || (latched && b < 8);
      bool hasV = false; float v = 0;
      if ((m == 0x01 || m == 0x02) && in.hasIout) { hasV = true; v = in.iout; }
      if (m == 0x20 && in.hasTemp) { hasV = true; v = in.temp; }
      cond({id, t, Sev::Critical, hasV, v, code, nullptr, "stm32", perm, 0, 0}, (in.fault & m) != 0, now, ts);
    }
    cond({"stm32_restarts_exhausted", Type::InverterFault, Sev::Critical, false, 0, "restarts_exhausted", nullptr, "stm32", true, 0, 0},
         (in.fault & kStatusRestartsExhausted) != 0, now, ts);
    // Overload: STM32 bit16 once this STM32 has shown it sets bit16 (framed link);
    // otherwise, on any link, the ESP rule (>= 19 A for 2 s). No STM32 sends
    // framed status yet, so today the ESP rule runs everywhere.
    if (in.framed && (in.fault & kStatusOverloadWarn)) bit16Seen_ = true;
    if (!in.framed) bit16Seen_ = false;
    if (bit16Seen_) {
      cond({"overload", Type::Overload, Sev::Warning, in.hasIout, in.iout, "stm32_warn_19a", nullptr, "stm32", false, 0, 0},
           (in.fault & kStatusOverloadWarn) != 0, now, ts);
    } else if (in.hasIout) {
      bool raw = hyst(isActive("overload"), in.iout, kOverloadAmps, kOverloadClearAmps);
      cond({"overload", Type::Overload, Sev::Warning, true, in.iout, "esp_19a_2s", nullptr, "esp", false, 2000, 5000}, raw, now, ts);
    }
    if (in.hasTemp) {
      bool raw = hyst(isActive("temp_warning"), in.temp, kTempWarnC, kTempWarnClearC);
      cond({"temp_warning", Type::OverTemp, Sev::Warning, true, in.temp, "inverter_70c", nullptr, "stm32", false, 2000, 10000}, raw, now, ts);
    }
    if (in.hasGrid) {
      bool raw = hystBelow(isActive("grid_down"), in.gridV, kGridMinV, kGridRestoreV);
      cond({"grid_down", Type::GridDown, Sev::Warning, true, in.gridV, "grid_below_160v", nullptr, "stm32", false, 3000, 3000}, raw, now, ts);
    }
  }
  // ---- Battery full ----------------------------------------------------------
  {
    bool flag = in.stmOnline && ((in.fault & kStatusChargerFull) || in.chr == ChargeState::Float);
    bool bmsFull = in.bmsOnline && in.socValid && in.soc >= 99.5f && fabsf(in.bmsCurrentA) < 1.0f;
    bool active = isActive("battery_full");
    bool raw = flag || bmsFull || (active && in.socValid && in.soc >= 97.0f);
    if (in.stmOnline || in.socValid) {
      const char *src = flag ? "stm32" : "bms";
      cond({"battery_full", Type::BatteryFull, Sev::Info, in.socValid, in.soc, flag ? "charger_full" : "soc_100", nullptr, src, false, 10000, 10000}, raw, now, ts);
    }
  }
  // ---- Battery low (SoC policy 20/10/5 %, hysteresis 3) -----------------------
  bat_.update(in.socValid, in.soc, now);
  {
    BatteryLevel l = bat_.level();
    Sev s = l == BatteryLevel::Low ? Sev::Warning : Sev::Critical;
    cond({"battery_soc", Type::BatteryLow, s, in.socValid, in.soc, batteryLevelType(l == BatteryLevel::None ? BatteryLevel::Low : l),
          nullptr, in.socSource, false, 0, 0}, l != BatteryLevel::None, now, ts);
  }
  // ---- BMS alarms ----------------------------------------------------------------
  if (in.bmsOnline) {
    for (int b = 0; b < 24; ++b) {
      const char *name = jk::alarmName(b);
      if (!name) continue;
      snprintf(id, sizeof id, "bms_%d", b);
      snprintf(code, sizeof code, "%s", name);
      BmsMap m = bmsMap(b);
      cond({id, m.type, m.sev, false, 0, code, nullptr, "bms", false, 0, 5000}, (in.bmsAlarms >> b) & 1u, now, ts);
    }
  }
  // ---- Battery critical (ESP SoC <= 10 % latch, STM32 reserve latch) -------------
  {
    cond({"battery_critical", Type::BatteryCritical, Sev::Critical, in.socValid, in.soc, "soc_10pct", nullptr, "esp", false, 0, 0, "charging"},
         in.critLatched, now, ts);
    if (in.stmOnline) {  // offline STM32: hold the last state
      cond({"stm32_reserve", Type::BatteryCritical, Sev::Critical, in.socValid, in.soc, "stm32_reserve_latch", nullptr, "stm32", false, 0, 0, "charge_to_52v"},
           in.stmReserve, now, ts);
    }
    // One output_restored, only once both latches are clear: the STM32 52 V release is the hardware floor.
    bool esp = isActive("battery_critical"), stm = isActive("stm32_reserve");
    if (esp || stm) { restorePending_ = true; lastLatchStm_ = stm; }
    else if (restorePending_) {
      restorePending_ = false;
      oneShot(Type::OutputRestored, Sev::Info, "output_restored", lastLatchStm_ ? "stm32_reserve_released" : "soc15_charging",
              lastLatchStm_ ? "stm32" : "esp", in.socValid, in.soc, ts);
    }
  }
  // ---- Smart sockets -----------------------------------------------------------
  for (size_t i = 0; i < in.nSockets && i < Inputs::kMaxSockets; ++i) {
    const SocketIn &s = in.sockets[i];
    if (!s.id[0]) continue;
    snprintf(id, sizeof id, "socket_oc_%s", s.id);
    if (s.online) {
      bool act = isActive(id);
      bool warnRaw = s.alertWarning || hyst(act, s.current, kSocketWarnA, kSocketWarnClearA);
      bool wasCrit = false;
      for (auto &c : c_) if (c.used && c.active && !strcmp(c.ev.id, id)) wasCrit = c.ev.severity == Sev::Critical;
      bool crit = s.alertOvercurrent || hyst(wasCrit, s.current, kSocketTripA, kSocketTripClearA);
      cond({id, Type::Overcurrent, crit ? Sev::Critical : Sev::Warning, true, s.current,
            crit ? (s.alertOvercurrent ? "socket_tripped_20a" : "socket_20a") : "socket_warn_16a",
            s.id, "socket", false, crit ? 0u : 1000u, 5000}, crit || warnRaw, now, ts);
    }
    // Lockout: critical, latched on the socket (NVS); clears only when the socket reports it reset.
    snprintf(id, sizeof id, "socket_lock_%s", s.id);
    if (s.online)
      cond({id, Type::ApplianceLockout, Sev::Critical, true, s.lockoutA, "socket_lockout_3trips", s.id, "socket", true, 0, 0},
           s.lockout, now, ts);
    snprintf(id, sizeof id, "socket_relay_%s", s.id);
    bool rej = !strcmp(s.cmd, "rejected"), to = !strcmp(s.cmd, "timeout"), ok = !strcmp(s.cmd, "confirmed");
    if (rej || to || ok)
      cond({id, Type::RelayFailed, Sev::Critical, false, 0, rej ? "hardware_rejected" : to ? "hardware_timeout" : "", s.id, "socket", false, 0, 0},
           rej || to, now, ts);
  }
}

void Engine::oneShot(Type t, Sev sev, const char *id, const char *code, const char *source, bool hasValue, float value, uint32_t ts) {
  Event e; e.type = t; e.active = true; e.severity = sev; e.hasValue = hasValue; e.value = value;
  snprintf(e.id, sizeof e.id, "%s", id); snprintf(e.code, sizeof e.code, "%s", code); e.source = source; e.ts = ts;
  push(e);
}

size_t Engine::drain(Event *out, size_t max) {
  size_t n = 0;
  while (outN_ && n < max) { out[n++] = out_[outHead_]; outHead_ = (outHead_ + 1) % kOut; --outN_; }
  return n;
}
size_t Engine::activeCount() const { size_t n = 0; for (auto &c : c_) n += c.used && c.active; return n; }
const Event *Engine::active(size_t i) const {
  for (auto &c : c_) if (c.used && c.active && i-- == 0) return &c.ev;
  return nullptr;
}
}  // namespace ev
}  // namespace pc
