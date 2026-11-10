#pragma once
#include <stddef.h>
#include <stdint.h>
#include "stm32_types.h"

// 2.5.4: host-testable helpers for the app's live view (source, status, output
// power, load %, model capacity, battery time-to-empty/full) and the ADS1115
// I2C back-off. No Arduino.
namespace pc {
namespace live {

// ---- Powercell model (factory-set, NVS pc_cfg/model) ----
// ASSUMPTION (Caleb 2026-11-04): usable continuous power = 90 % of rated kVA
// (5 kVA -> 4.5 kW, 3.5 kVA -> 3.15 kW = 3150 W). Confirm the derating per model with hardware.
struct Model { const char *code; float ratedKva; float usableKw; };
// Accepts "PC3K", "PC3_5K", "PC5K", "PC10K" (case-insensitive; "5K", "5KVA",
// "PC5KVA", "3.5K", "PC3.5K", "35K", "PC35K" too). Returns nullptr when unknown or empty.
const Model *findModel(const char *code);
const Model *models(size_t &count);

enum class Source : uint8_t { None, Battery, Grid, Solar };
enum class Status : uint8_t { Unknown, Standby, Idle, Inverting, Charging, Fault };
const char *sourceName(Source s);   // "none" | "battery" | "grid" | "solar"
const char *statusName(Status s);   // "unknown" | "standby" | "idle" | "inverting" | "charging" | "fault"

struct Inputs {
  bool stmFresh = false;
  SysState sys = SysState::Unknown;
  SwitchPos sw = SwitchPos::Unknown;
  ChargeState chr = ChargeState::Unknown;
  bool faultActive = false;
  bool hasVout = false, hasIout = false;
  float vout = 0, iout = 0;
  bool bmsOn = false;
  float bmsCurrentA = 0;   // + charging, - discharging
  float bmsPowerW = 0;     // signed like the current
  bool solarOk = false;
  float solarW = 0;
};
constexpr float kSolarActiveW = 20.0f;      // PV counts as a source above this
constexpr float kChargeA = 0.5f;            // BMS current that counts as charging
constexpr float kInverterEff = 0.90f;       // ASSUMPTION for the BMS-based output estimate

Source source(const Inputs &in);
Status status(const Inputs &in);

enum class PowerSrc : uint8_t { None, Stm32, BmsEstimate, Off };
const char *powerSrcName(PowerSrc p);       // null-able in JSON when None
// Output (AC) power in W. Inverting: STM32 VOUT x IOUT (apparent VA), else
// |BMS discharge W| x kInverterEff. Standby/idle on battery (inverter off): 0.
// Grid pass-through or no data: returns PowerSrc::None (unknown).
PowerSrc outputPowerW(const Inputs &in, float &w);
// Percent of usable power drawn (0..>100), false when model or power unknown.
bool loadPct(float outW, const Model *m, float &pct);

// ---- Battery runtime (time to empty / full) ----
// Current is smoothed with a time-based EMA (tau 60 s) so a short spike does
// not swing the estimate. Below kIdleA the pack counts as idle (no estimate).
class Runtime {
 public:
  static constexpr float kTauS = 60.0f, kIdleA = 0.3f, kMaxMin = 99.0f * 60.0f;  // cap 99 h
  void update(float currentA, float remainAh, float fullAh, uint32_t nowMs);
  void reset() { have_ = false; }
  float smoothedA() const { return ema_; }
  bool toEmptyMin(float &min) const;   // discharging only
  bool toFullMin(float &min) const;    // charging only
 private:
  bool have_ = false;
  float ema_ = 0, remain_ = 0, full_ = 0;
  uint32_t at_ = 0;
};

// ---- I2C back-off ----
// After kFailThreshold consecutive failures the reader waits 1 s, 2 s, 4 s ...
// up to kMaxMs between attempts. One "went bad" and one "recovered" edge.
class Backoff {
 public:
  static constexpr uint8_t kFailThreshold = 3;
  static constexpr uint32_t kBaseMs = 1000, kMaxMs = 30000;
  bool due(uint32_t nowMs) const { return !waiting_ || (int32_t)(nowMs - next_) >= 0; }
  // Returns true exactly once when the device is declared failed (log WARN).
  bool fail(uint32_t nowMs);
  // Returns true exactly once when a failed device answers again (log INFO).
  bool ok();
  bool failed() const { return failed_; }
  uint32_t delayMs() const { return delay_; }
  uint32_t consecutive() const { return fails_; }
 private:
  uint32_t fails_ = 0, delay_ = 0, next_ = 0;
  bool failed_ = false, waiting_ = false;
};

// ---- 2.5.7 PV view (live block, SOLAR console, SELFTEST) ----
// valid only for an ok reading with finite V/A; power = V x A. Invalid -> all
// three null in JSON. ageMs = last good read age, UINT32_MAX = never.
struct SolarView { bool valid = false; float voltage = 0, current = 0, powerW = 0; };
SolarView solarView(bool ok, float voltage, float current);
// "SOLAR valid=1 V=41.20 A=3.105 W=127.9 i2c_errors=18 failing=0 age_ms=230"
// (V/A/W print "null" when invalid, age_ms "never" when no good read since boot).
size_t formatSolarLine(char *buf, size_t n, const SolarView &v, uint32_t i2cErrors, bool failing, uint32_t ageMs);
// 2.5.8: same line plus " today_kwh=x.xxx total_kwh=x.xxx" ("null" when energy < 0 = unknown).
size_t formatSolarLine2(char *buf, size_t n, const SolarView &v, uint32_t i2cErrors, bool failing, uint32_t ageMs,
                        double todayKwh, double totalKwh);
// 2.5.8: the installed app (1.0.39/1.0.40) reads GET /data root `solar.power`
// through displayKw(): values > 20 are taken as W, <= 20 as kW. Sending kW
// (W / 1000) is shown right for any PV power under 20 kW, including small W.
inline float appPowerKw(float w) { return w / 1000.0f; }
}  // namespace live
}  // namespace pc
