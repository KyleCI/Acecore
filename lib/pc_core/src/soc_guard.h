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
//    fw 2.5.3: OR the BMS is online with valid data and its own SoC >= 15 % held 60 s
//    (idle is enough). Fixes a stale NVS latch never clearing on a full idle pack.
//    STM32/ADC voltage-derived SoC never releases without charging.
//  - The STM32 reserve latch (legacy RSV:1, 47.5 V for 60 s) is the hardware
//    backstop; it releases at Vbat >= 52.0 V held 60 s from any source (solar or grid). That is the floor:
//    output stays off and output_restored waits until it clears too.
//  - fw 2.5.5 (Caleb Nov 4 2026, "52 V is too high to refuse to come on"): a user/cloud
//    ON is gated by battery percentage, not the 52 V latch: refused only when the BMS
//    SoC is <= 10 %. Above that an ON clears the ESP latch and is sent even if the
//    STM32 reports RSV:1 (the STM32 itself still holds the inverter off until its own
//    52 V release; that threshold lives in STM32 config.h, see change notes).
//    BMS offline/invalid: fallback is pack voltage >= 48.0 V (16S LFP, 3.00 V/cell,
//    about 10 %) from the STM32; no voltage either: old rule (refuse while latched).
//    Automatic restore after a latch keeps the 15 %-held-60 s hysteresis.
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
  bool bmsSocValid = false;                        // soc above comes from a valid BMS frame
};
enum class Change : uint8_t { None, Latched, Released };

class CriticalGuard {
 public:
  void restore(bool latched) { latched_ = latched; chargeSince_ = 0; charging_ = false; bmsOk_ = false; bmsOkSince_ = 0; }
  bool latched() const { return latched_; }
  bool chargingHeld(uint32_t now) const { return charging_ && now - chargeSince_ >= kChargeHoldMs; }
  bool bmsHealthyHeld(uint32_t now) const { return bmsOk_ && now - bmsOkSince_ >= kChargeHoldMs; }
  Change update(const GuardIn &in, uint32_t now) {
    bool chg = in.bmsOnline ? in.bmsCurrentA > kChargeA : in.stmCharging;
    if (chg && !charging_) chargeSince_ = now;
    charging_ = chg;
    bool ok = in.bmsOnline && in.bmsSocValid && in.socValid && in.soc >= kRestoreSoc;
    if (ok && !bmsOk_) bmsOkSince_ = now;
    bmsOk_ = ok;
    if (!latched_) {
      if (in.socValid && in.soc <= kCritical) { latched_ = true; return Change::Latched; }
      return Change::None;
    }
    if (in.socValid && in.soc >= kRestoreSoc && (chargingHeld(now) || bmsHealthyHeld(now))) { latched_ = false; return Change::Released; }
    return Change::None;
  }
 private:
  bool latched_ = false, charging_ = false, bmsOk_ = false;
  uint32_t chargeSince_ = 0, bmsOkSince_ = 0;
};

// Power-on gate for user/cloud ON (2.5.5).
constexpr float kOnMinSoc = kCritical;   // refuse at or below 10 %
constexpr float kOnFallbackV = 48.0f;    // 16S LFP ~10 %, only when the BMS is offline/invalid
struct OnGateIn {
  bool bmsSocValid = false; float bmsSoc = 0;
  bool vbatValid = false; float vbat = 0;
  bool espLatched = false, stmReserve = false;
};
enum class OnGate : uint8_t { Allow, RefuseSoc, RefuseVoltage, RefuseLatched };
// 2.5.13 (Caleb 8 Nov 2026, "restart blocked until back to 15 %"): while the ESP latch
// holds, an ON needs BMS SoC >= 15 % (kRestoreSoc), the same rule as the DSP images.
// Not latched: > 10 % as before. The STM32 reserve latch alone keeps the 2.5.5 rule.
inline OnGate onGate(const OnGateIn &in) {
  if (in.bmsSocValid) {
    if (in.espLatched) return in.bmsSoc >= kRestoreSoc ? OnGate::Allow : OnGate::RefuseSoc;
    return in.bmsSoc > kOnMinSoc ? OnGate::Allow : OnGate::RefuseSoc;
  }
  if (in.vbatValid) return in.vbat >= kOnFallbackV ? OnGate::Allow : OnGate::RefuseVoltage;
  return (in.espLatched || in.stmReserve) ? OnGate::RefuseLatched : OnGate::Allow;
}
}  // namespace soc
}  // namespace pc
