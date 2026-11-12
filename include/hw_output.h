#pragma once

#include <stddef.h>
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

// Pad operations for one GPIO15 write on the DSP board, in order.
// GPIO15 resets into its IO_MUX JTAG function (MTDO, FUNC 0). gpio_set_level
// and gpio_set_direction only touch the GPIO matrix, so without SelectGpio the
// pad never follows the written level and the inverter enable cannot switch.
// Arduino pinMode() (gpio_config) does this select; Dubem's firmware uses
// pinMode(PWR_PIN, OUTPUT) + digitalWrite and works on the same board.
enum class PadOp : uint8_t { HoldRelease = 0, SelectGpio, PullsOff, SetLevel, OutputEnable, HoldEnable };
struct PadStep {
  PadOp op;
  int level;  // SetLevel only, else -1
};
constexpr size_t kDspPadSteps = 7;
// Release any hold, select the GPIO function, set the level before and after
// enabling the driver (no glitch), and hold the pad only at the off level.
inline size_t v011_dsp_pad_steps(bool logical_on, PadStep out[kDspPadSteps]) {
  const int level = v011_dsp_level(logical_on);
  size_t n = 0;
  out[n++] = {PadOp::HoldRelease, -1};
  out[n++] = {PadOp::SetLevel, level};
  out[n++] = {PadOp::SelectGpio, -1};
  out[n++] = {PadOp::PullsOff, -1};
  out[n++] = {PadOp::OutputEnable, -1};
  out[n++] = {PadOp::SetLevel, level};
  if (!logical_on) out[n++] = {PadOp::HoldEnable, -1};
  return n;
}

// DSP front light on GPIO27 (NPN driver, HIGH = ON). It follows the inverter
// exactly as Dubem's firmware does (provisioning.cpp applyDeviceState:
// digitalWrite(FRONT_LIGHT_PIN, deviceState ? HIGH : LOW)). Caleb approved
// 2026-11-10 21:10 WAT. GPIO27 is a plain GPIO (no JTAG function), so
// pinMode/digitalWrite is enough. The original profile has no front light.
constexpr int kDspFrontLightV011 = 27;
inline int v011_front_light_level(bool inverter_on) { return inverter_on ? 1 : 0; }

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
