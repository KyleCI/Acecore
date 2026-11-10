#pragma once

#include <stdint.h>

namespace powercell {

// GPIO15 on the v0.1.1 DSP board drives the inverter enable. The transistor
// needs HIGH on its base, so HIGH = ON and LOW = OFF. Caleb confirmed that
// on 2026-11-10 11:53 WAT, matching Dubem's pinDefines.h and superseding the
// earlier LOW = ON choice. DSP_PWR_ACTIVE_LEVEL is the only switch.
// 1 means HIGH = ON. The STM32 profile does not use this macro.
#ifndef DSP_PWR_ACTIVE_LEVEL
#define DSP_PWR_ACTIVE_LEVEL 1
#endif
static_assert(DSP_PWR_ACTIVE_LEVEL == 0 || DSP_PWR_ACTIVE_LEVEL == 1, "DSP_PWR_ACTIVE_LEVEL is 0 or 1");
constexpr int kDspEnableV011 = 15;
constexpr bool kDspPolarityUserConfirmed = true;  // HIGH=ON, Caleb 2026-11-10 11:53 WAT
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

// v0.1.1 maps a battery hold to the DSP off level (GPIO15 low). The original
// profile does not drive that pin.
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

// SD chip select. DSP hardware uses GPIO5 (Dubem). GPIO33 is unused on that
// board. The original profile keeps GPIO14.
inline int profile_sd_cs(HardwareProfile profile) {
  return profile == HardwareProfile::V011 ? 5 : 14;
}

// Every DSP "off" command uses the same pin level. The original profile's
// relay is active-high, so its off level stays 0 and is not GPIO15-as-DSP.
enum class OffOrigin : uint8_t {
  BootLatch = 0,
  OtaSafe,
  BatteryCutoff,
  Overload,
  PaymentLock,
  RemoteOff,
  FactoryReset
};
inline int commanded_pin_level(HardwareProfile profile, bool logical_on) {
  return profile == HardwareProfile::V011 ? v011_dsp_level(logical_on) : original_relay_level(logical_on);
}
inline int off_origin_level(HardwareProfile profile, OffOrigin) {
  return commanded_pin_level(profile, false);
}

inline bool diag_kind_allowed(HardwareProfile profile, uint8_t kind) {
  if (profile == HardwareProfile::V011 && (kind == 13 || kind == 14)) return false;
  return true;
}

}  // namespace powercell
