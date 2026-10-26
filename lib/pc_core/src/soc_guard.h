#pragma once
#include <stdint.h>
// SoC rules, Caleb Oct 22 2026 (fw 2.2). Pure logic, host tested.
//  - Socket shedding (INTERIM until per-socket config storage exists; every
//    socket defaults to Heavy): Heavy off at <= 40 %, back on at >= 50 %;
//    Essential off at <= 20 %, back on at >= 30 %.
//  - Critical latch: SoC <= 10 % (BMS, STM32 voltage fallback) shuts the inverter
//    output off and latches (NVS). Release: SoC >= 15 % (Caleb, was 25) AND charging, i.e. BMS pack
//    current > +1 A held for 60 s (grid or solar). Without the BMS, an STM32 charge
//    state (BULK/ABSORB/FLOAT) held 60 s counts as charging.
//  - The STM32 reserve latch (legacy RSV:1, 47.5 V for 60 s) is the hardware
//    backstop; it releases at Vbat >= 52.0 V held 60 s from any source (solar or grid). That is the floor:
//    output stays off and output_restored waits until it clears too.
namespace pc {
namespace soc {
enum class Priority : uint8_t { Heavy = 0, Essential = 1 };
constexpr float kHeavyOff = 40, kHeavyOn = 50, kEssentialOff = 20, kEssentialOn = 30;
constexpr float kCritical = 10, kRestoreSoc = 15, kChargeA = 1.0f;
constexpr uint32_t kChargeHoldMs = 60000;

// Hysteresis per socket. soc invalid: keep the previous decision.
inline bool shed(Priority p, bool wasShed, bool socValid, float soc) {
  if (!socValid) return wasShed;
  float off = p == Priority::Essential ? kEssentialOff : kHeavyOff;
  float on = p == Priority::Essential ? kEssentialOn : kHeavyOn;
  return wasShed ? soc < on : soc <= off;
}

struct GuardIn {
  bool socValid = false; float soc = 0;
  bool bmsOnline = false; float bmsCurrentA = 0;  // + = charging
  bool stmCharging = false;                        // STM32 charger BULK/ABSORB/FLOAT (fallback)
};
enum class Change : uint8_t { None, Latched, Released };

class CriticalGuard {
 public:
  void restore(bool latched) { latched_ = latched; chargeSince_ = 0; charging_ = false; }
  bool latched() const { return latched_; }
  bool chargingHeld(uint32_t now) const { return charging_ && now - chargeSince_ >= kChargeHoldMs; }
  Change update(const GuardIn &in, uint32_t now) {
    bool chg = in.bmsOnline ? in.bmsCurrentA > kChargeA : in.stmCharging;
    if (chg && !charging_) chargeSince_ = now;
    charging_ = chg;
    if (!latched_) {
      if (in.socValid && in.soc <= kCritical) { latched_ = true; return Change::Latched; }
      return Change::None;
    }
    if (in.socValid && in.soc >= kRestoreSoc && chargingHeld(now)) { latched_ = false; return Change::Released; }
    return Change::None;
  }
 private:
  bool latched_ = false, charging_ = false;
  uint32_t chargeSince_ = 0;
};
}  // namespace soc
}  // namespace pc
