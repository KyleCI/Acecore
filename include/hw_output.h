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

}  // namespace powercell
