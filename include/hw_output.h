#pragma once

#include <stdint.h>

namespace powercell {

// GPIO15 on hardware v0.1.1 is the DSP/AC enable, active low.
// Low is on and high is off. That polarity is user-confirmed, not a
// measurement that charging stays up. The original collector relay is
// active high on the same pin and is not this function.
constexpr int kDspEnableV011 = 15;
constexpr bool kDspPolarityUserConfirmed = true;
constexpr bool kPhysicalShutdownVerified = false;

inline int v011_dsp_level(bool logical_on) { return logical_on ? 0 : 1; }
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

// SD chip select. v0.1.1 uses GPIO33. The original profile keeps GPIO14
// because GPIO33 is the STM32 RX pin.
inline int profile_sd_cs(HardwareProfile profile) {
  return profile == HardwareProfile::V011 ? 33 : 14;
}

inline bool diag_kind_allowed(HardwareProfile profile, uint8_t kind) {
  if (profile == HardwareProfile::V011 && (kind == 13 || kind == 14)) return false;
  return true;
}

}  // namespace powercell
