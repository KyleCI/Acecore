#pragma once

#include <stdint.h>

// DSP v0.1.1 system temperature (10k NTC) and cooling fan.
// Ported from Dubem's "dspic collector_2" (2026-11-11): include/thermal.h,
// src/thermal.cpp, include/pinDefines.h (FAN_PIN 25, NTC_PIN 36).
//
// Divider from 3V3, centre tap to GPIO36 (VP, ADC1, input only):
//   NTC_ON_LOW_SIDE 0 (Dubem default): 3V3 --[NTC]--+--[2k7]-- GND, pin volts rise when hot
//   NTC_ON_LOW_SIDE 1                : 3V3 --[2k7]--+--[NTC]-- GND, pin volts fall when hot
// Dubem recommends 100 nF from GPIO36 to GND.
//
// Fan on GPIO25, NPN/MOSFET low-side driver, HIGH = ON. Hysteresis:
// ON at >= 70.0 C, OFF at <= 65.0 C. Fail-safe: a sensor fault (open, short or
// out of 0..150 C) forces the fan ON. Dubem's code has no shutdown or
// derating on temperature, and neither does this port.
namespace powercell {
namespace thermal {

#ifndef NTC_ON_LOW_SIDE
#define NTC_ON_LOW_SIDE 0
#endif

constexpr float kFanOnC = 70.0f;
constexpr float kFanOffC = 65.0f;
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
