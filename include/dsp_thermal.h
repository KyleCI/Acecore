#pragma once

#include <stdint.h>

// DSP v0.1.1 system temperature (10k NTC) and cooling fan.
// Ported from Dubem's "dspic collector_2" (2026-11-11): include/thermal.h,
// src/thermal.cpp, include/pinDefines.h (FAN_PIN 25, NTC_PIN 36).
// Caleb 2026-11-11 18:30 WAT: the NTC moved to GPIO33, fan OFF at 60 C,
// over-temperature cutoff and alert at 80 C.
//
// Divider from 3V3, centre tap to GPIO33 (ADC1_CH5):
//   NTC_ON_LOW_SIDE 0 (Dubem default): 3V3 --[NTC]--+--[2k7]-- GND, pin volts rise when hot
//   NTC_ON_LOW_SIDE 1                : 3V3 --[2k7]--+--[NTC]-- GND, pin volts fall when hot
// Dubem recommends 100 nF from GPIO36 to GND.
//
// Fan on GPIO25, NPN/MOSFET low-side driver, HIGH = ON. Hysteresis:
// ON at >= 70.0 C, OFF at <= 60.0 C. Fail-safe: a sensor fault (open, short or
// out of 0..150 C) forces the fan ON.
//
// Over-temperature cutoff (not in Dubem's code; Caleb's decision): a valid
// reading >= 80.0 C latches the cutoff, the AC output (GPIO15) is driven OFF
// and an over_temp alert is raised. The latch is saved in NVS. ON is refused
// while latched until a valid reading is below 70.0 C; then a user ON clears
// the latch. Nothing turns the output back on by itself. A sensor fault alone
// never cuts off (fan forced ON and a temp_sensor_fault alert instead).
namespace powercell {
namespace thermal {

#ifndef NTC_ON_LOW_SIDE
#define NTC_ON_LOW_SIDE 0
#endif

constexpr float kFanOnC = 70.0f;
constexpr float kFanOffC = 60.0f;
constexpr float kCutoffC = 80.0f;      // latch the over-temperature cutoff at or above this
constexpr float kOnAllowedBelowC = 70.0f;  // a latched unit accepts a user ON only below this
constexpr uint32_t kPeriodMs = 2000;
constexpr uint32_t kStaleMs = 10000;
constexpr int kSamples = 16;

constexpr float kR25Ohm = 10000.0f;
constexpr float kBeta = 3950.0f;
constexpr float kRFixedOhm = 2700.0f;
constexpr float kVccMv = 3300.0f;
constexpr float kFaultMv = 40.0f;  // within this of 0 V or VCC = open or short
constexpr float kMinC = 0.0f;
constexpr float kMaxC = 150.0f;

// Divider millivolts -> deg C. Returns false (open, short, non-finite or out
// of kMinC..kMaxC) and leaves *c untouched. Same math as Dubem's ntcMvToCelsius.
bool mvToCelsius(float mv, float *c, bool ntc_low_side = NTC_ON_LOW_SIDE != 0);

// Dubem's fan rule. fault forces ON. Between the thresholds the fan keeps its state.
bool fanWanted(bool fan_on, bool fault, float c);

// Over-temperature cutoff latch. Pure logic; the device glue persists it and
// drives GPIO15. valid/c are the fresh NTC reading (c ignored when !valid).
class OverTempGuard {
 public:
  void restore(bool latched) { latched_ = latched; dirty_ = false; }
  // True on the edge that latches the cutoff.
  bool update(bool valid, float c) {
    if (valid && c >= kCutoffC && !latched_) { latched_ = true; dirty_ = true; return true; }
    return false;
  }
  bool latched() const { return latched_; }
  // ON would be refused now. Hot (>= 80 C) always refuses. Latched refuses
  // until a valid reading is below 70 C (no reading / fault = refuse).
  bool blocksOn(bool valid, float c) const {
    if (valid && c >= kCutoffC) return true;
    if (latched_) return !(valid && c < kOnAllowedBelowC);
    return false;
  }
  // A user ON: returns true when allowed, and clears the latch if it was set.
  bool acceptOn(bool valid, float c) {
    if (blocksOn(valid, c)) return false;
    if (latched_) { latched_ = false; dirty_ = true; }
    return true;
  }
  bool dirty() const { return dirty_; }
  void ackSaved() { dirty_ = false; }

 private:
  bool latched_ = false;
  bool dirty_ = false;
};

// Sensor health for the temp_sensor_fault alert: a sample was taken and it was
// faulted, or the last sample is stale (the task stopped). Before the first
// sample (boot) there is no fault.
inline bool sensorFaulted(bool sampled, bool fault, uint32_t age_ms) {
  return sampled && (fault || age_ms >= kStaleMs);
}

}  // namespace thermal
}  // namespace powercell

struct DspThermalReading {
  bool valid = false;    // fresh, plausible reading
  bool fault = false;    // last sample was open, short or out of range
  bool sampled = false;  // the task has taken at least one sample
  float temp_c = 0;      // only meaningful when valid
  float mv = 0;          // last averaged pin millivolts (bench check)
  bool fan_on = false;
  uint32_t age_ms = 0;
};

void dspThermalBegin();
// True when a fresh valid reading exists. *out is always filled (fan state,
// fault, mv) when the module is running.
bool dspThermalRead(DspThermalReading *out);
