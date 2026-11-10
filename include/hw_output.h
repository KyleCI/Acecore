#pragma once

#include <stdint.h>

namespace powercell {

// GPIO15 on the DSP board drives an NPN low-side transistor.
// Dubem's pinDefines.h comment says HIGH = ON. Caleb overruled that on
// 2026-11-10: the shipped level stays LOW = ON. DSP_PWR_ACTIVE_LEVEL is the
// only switch. 0 means LOW = ON. Do not change it without a new confirmation.
#ifndef DSP_PWR_ACTIVE_LEVEL
#define DSP_PWR_ACTIVE_LEVEL 0
#endif
static_assert(DSP_PWR_ACTIVE_LEVEL == 0 || DSP_PWR_ACTIVE_LEVEL == 1, "DSP_PWR_ACTIVE_LEVEL is 0 or 1");
constexpr int kDspEnableV011 = 15;
constexpr bool kDspPolarityUserConfirmed = true;  // LOW=ON, confirmed 2026-11-10
constexpr bool kPhysicalShutdownVerified = false;

inline int v011_dsp_level(bool logical_on) {
  return logical_on ? DSP_PWR_ACTIVE_LEVEL : (DSP_PWR_ACTIVE_LEVEL ^ 1);
}
inline int original_relay_level(bool logical_on) { return logical_on ? 1 : 0; }

enum class BootAction : uint8_t { SetOutputLatch = 0, EnableOutput = 1 };
struct BootStep {
  BootAction action;
  int level;
};

// Latch the off level before the pin becomes an output.
inline void v011_dsp_boot(BootStep out[2]) {
  out[0] = {BootAction::SetOutputLatch, v011_dsp_level(false)};
  out[1] = {BootAction::EnableOutput, v011_dsp_level(false)};
}

inline bool battery_recovery_drives_on() { return false; }

enum class HardwareProfile : uint8_t { Original = 0, V011 = 1 };

// v0.1.1 maps a battery hold to GPIO15 high. The original profile does not.
// Recovery never requests on. Charging and the BMS FET registers are not written.
inline bool battery_ac_output_auto_off(HardwareProfile profile) {
  return profile == HardwareProfile::V011;
}
inline int battery_cutoff_pin_level(HardwareProfile profile, bool hold_off) {
  if (!hold_off || profile != HardwareProfile::V011) return -1;
  return v011_dsp_level(false);
}
inline bool battery_cutoff_writes_bms_fets() { return false; }
inline bool battery_cutoff_stops_charging() { return false; }

// SD chip select. DSP hardware uses GPIO5 (Dubem). GPIO33 is PZEM TX on that
// board. The original profile keeps GPIO14.
inline int profile_sd_cs(HardwareProfile profile) {
  return profile == HardwareProfile::V011 ? 5 : 14;
}

inline bool diag_kind_allowed(HardwareProfile profile, uint8_t kind) {
  if (profile == HardwareProfile::V011 && (kind == 13 || kind == 14)) return false;
  return true;
}

}  // namespace powercell
