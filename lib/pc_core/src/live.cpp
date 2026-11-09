#include <stdio.h>
#include "live.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

namespace pc {
namespace live {
namespace {
const Model kModels[] = {
  {"PC3K", 3.0f, 2.7f},
  {"PC5K", 5.0f, 4.5f},
  {"PC10K", 10.0f, 9.0f},
};
bool charging(const Inputs &in) {
  if (in.chr == ChargeState::Bulk || in.chr == ChargeState::Absorb || in.chr == ChargeState::Float) return true;
  return in.bmsOn && in.bmsCurrentA > kChargeA;
}
}  // namespace

const Model *models(size_t &count) { count = sizeof kModels / sizeof kModels[0]; return kModels; }

const Model *findModel(const char *code) {
  if (!code) return nullptr;
  char n[12] = {}; size_t k = 0;
  for (const char *p = code; *p && k < sizeof n - 1; ++p) if (!isspace((unsigned char)*p)) n[k++] = (char)toupper((unsigned char)*p);
  if (!k) return nullptr;
  if (k >= 3 && !strcmp(n + k - 3, "KVA")) { n[k - 2] = 0; k -= 2; }   // 5KVA -> 5K
  const char *b = !strncmp(n, "PC", 2) ? n + 2 : n;
  for (const Model &m : kModels) if (!strcmp(b, m.code + 2)) return &m;
  return nullptr;
}

const char *sourceName(Source s) {
  switch (s) { case Source::Battery: return "battery"; case Source::Grid: return "grid"; case Source::Solar: return "solar"; default: return "none"; }
}
const char *statusName(Status s) {
  switch (s) { case Status::Standby: return "standby"; case Status::Idle: return "idle"; case Status::Inverting: return "inverting";
    case Status::Charging: return "charging"; case Status::Fault: return "fault"; default: return "unknown"; }
}
const char *powerSrcName(PowerSrc p) {
  switch (p) { case PowerSrc::Stm32: return "stm32"; case PowerSrc::BmsEstimate: return "bms_estimate"; case PowerSrc::Off: return "off"; default: return nullptr; }
}

Source source(const Inputs &in) {
  bool pv = in.solarOk && in.solarW >= kSolarActiveW;
  if (!in.stmFresh) return pv ? Source::Solar : Source::None;
  if (in.sys == SysState::Inverter) {
    // PV covering the load (battery not discharging) -> solar, else battery.
    if (pv && in.bmsOn && in.bmsCurrentA >= 0) return Source::Solar;
    return Source::Battery;
  }
  if (in.sys == SysState::Grid) return Source::Grid;
  return pv ? Source::Solar : Source::None;
}

Status status(const Inputs &in) {
  if (!in.stmFresh) return Status::Unknown;
  if (in.sys == SysState::Fault || in.faultActive) return Status::Fault;
  if (in.sys == SysState::Inverter) return Status::Inverting;
  if (charging(in)) return Status::Charging;
  if (in.sys == SysState::Grid) return Status::Idle;   // grid pass-through, not charging
  if (in.sys == SysState::Idle || in.sys == SysState::Reserve) return Status::Standby;
  return Status::Unknown;
}

PowerSrc outputPowerW(const Inputs &in, float &w) {
  w = 0;
  if (!in.stmFresh) return PowerSrc::None;
  if (in.sys == SysState::Inverter) {
    if (in.hasVout && in.hasIout) { w = fabsf(in.vout * in.iout); return PowerSrc::Stm32; }
    if (in.bmsOn && in.bmsPowerW < 0) { w = -in.bmsPowerW * kInverterEff; return PowerSrc::BmsEstimate; }
    if (in.bmsOn) { w = 0; return PowerSrc::BmsEstimate; }
    return PowerSrc::None;
  }
  if (in.sys == SysState::Idle || in.sys == SysState::Reserve || in.sys == SysState::Fault) return PowerSrc::Off;  // output off
  return PowerSrc::None;  // grid pass-through: no AC load meter on this side
}

bool loadPct(float outW, const Model *m, float &pct) {
  if (!m || m->usableKw <= 0) return false;
  pct = outW / (m->usableKw * 1000.0f) * 100.0f;
  return true;
}

void Runtime::update(float a, float remainAh, float fullAh, uint32_t now) {
  remain_ = remainAh; full_ = fullAh;
  if (!have_) { ema_ = a; at_ = now; have_ = true; return; }
  float dt = (now - at_) / 1000.0f;
  at_ = now;
  if (dt <= 0) return;
  if (dt > 600 || (a > kIdleA && ema_ < -kIdleA) || (a < -kIdleA && ema_ > kIdleA)) { ema_ = a; return; }  // gap or direction change
  float alpha = dt / (kTauS + dt);
  ema_ += alpha * (a - ema_);
}

bool Runtime::toEmptyMin(float &m) const {
  if (!have_ || ema_ >= -kIdleA || remain_ <= 0) return false;
  m = remain_ / -ema_ * 60.0f;
  if (m > kMaxMin) m = kMaxMin;
  return true;
}

bool Runtime::toFullMin(float &m) const {
  if (!have_ || ema_ <= kIdleA || full_ <= 0) return false;
  float need = full_ - remain_;
  if (need < 0) need = 0;
  m = need / ema_ * 60.0f;
  if (m > kMaxMin) m = kMaxMin;
  return true;
}

bool Backoff::fail(uint32_t now) {
  ++fails_;
  if (fails_ < kFailThreshold) return false;
  delay_ = delay_ ? (delay_ * 2 > kMaxMs ? kMaxMs : delay_ * 2) : kBaseMs;
  next_ = now + delay_;
  waiting_ = true;
  if (failed_) return false;
  failed_ = true;
  return true;
}

bool Backoff::ok() {
  bool rec = failed_;
  fails_ = 0; delay_ = 0; waiting_ = false; failed_ = false;
  return rec;
}
SolarView solarView(bool ok, float voltage, float current) {
  SolarView v;
  if (!ok || !isfinite(voltage) || !isfinite(current)) return v;
  v.valid = true; v.voltage = voltage; v.current = current; v.powerW = voltage * current;
  return v;
}

size_t formatSolarLine(char *buf, size_t n, const SolarView &v, uint32_t i2cErrors, bool failing, uint32_t ageMs) {
  char vs[16] = "null", as[16] = "null", ws[16] = "null", ag[16] = "never";
  if (v.valid) {
    snprintf(vs, sizeof vs, "%.2f", v.voltage); snprintf(as, sizeof as, "%.3f", v.current);
    snprintf(ws, sizeof ws, "%.1f", v.powerW);
  }
  if (ageMs != UINT32_MAX) snprintf(ag, sizeof ag, "%lu", (unsigned long)ageMs);
  int r = snprintf(buf, n, "SOLAR valid=%d V=%s A=%s W=%s i2c_errors=%lu failing=%d age_ms=%s", v.valid ? 1 : 0, vs, as, ws,
                   (unsigned long)i2cErrors, failing ? 1 : 0, ag);
  return r < 0 ? 0 : size_t(r);
}
size_t formatSolarLine2(char *buf, size_t n, const SolarView &v, uint32_t i2cErrors, bool failing, uint32_t ageMs,
                        double todayKwh, double totalKwh) {
  size_t k = formatSolarLine(buf, n, v, i2cErrors, failing, ageMs);
  if (k >= n) return k;
  char t[20] = "null", T[20] = "null";
  if (todayKwh >= 0) snprintf(t, sizeof t, "%.3f", todayKwh);
  if (totalKwh >= 0) snprintf(T, sizeof T, "%.3f", totalKwh);
  int r = snprintf(buf + k, n - k, " today_kwh=%s total_kwh=%s", t, T);
  return r < 0 ? k : k + size_t(r);
}
}  // namespace live
}  // namespace pc
