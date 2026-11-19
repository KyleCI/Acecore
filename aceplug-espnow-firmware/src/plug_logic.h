#pragma once
// AcePlug ESP-NOW 1.1.0: current thresholds and flag packing. Pure logic, host tested
// (test/host/test_main.cpp test_aceplug_espnow). Thresholds from the LoRa AcePlug firmware
// (537d7f0, Oct 26 2026: fitted SANYOU relay is 10 A; 1.0.3 noise floor).
#include <math.h>
#include <stdint.h>
#include "espnow_wire.h"
namespace aceplug {
constexpr float kTripAmps = 10.0f;        // hard trip, relay off
constexpr float kWarnAmps = 8.0f;         // warning on at >= 8 A
constexpr float kWarnClearAmps = 7.0f;    // warning off below 7 A
constexpr uint32_t kRecloseMs = 15000;    // auto reclose 15 s after a trip (not when locked out)
constexpr float kNoiseA = 0.70f;          // idle RMS of ZMCT103 + ADC35 (measured ~0.68 A)
constexpr float kNoiseFloorA = 0.15f;
// Noise is uncorrelated with the load: remove it in quadrature, then floor.
inline float denoise(float irms) {
  if (!isfinite(irms) || irms <= 0) return 0;
  float sq = irms * irms - kNoiseA * kNoiseA;
  float a = sq > 0 ? sqrtf(sq) : 0;
  return a < kNoiseFloorA ? 0 : a;
}
inline bool shouldTrip(float amps) { return amps >= kTripAmps; }
inline bool warnState(bool previous, float amps) { return previous ? amps >= kWarnClearAmps : amps >= kWarnAmps; }
// v1 telemetry flags read by collector 2.5.40: bit0 command rejected, bit1 overcurrent (a lockout is
// reported as overcurrent: v1 has no lockout bit), restart reason in bits 2..7.
inline uint8_t telemetryFlags(bool rejected, bool overcurrent, bool lockout, uint8_t resetReason) {
  return (rejected ? pc::espnow::TelemetryRejected : 0) | ((overcurrent || lockout) ? pc::espnow::TelemetryOvercurrent : 0) |
         pc::espnow::restartFlags(resetReason);
}
// Optional Status (type 9) flags. Collector 2.5.40 decodes and ignores them; kept for a later collector.
inline uint8_t statusFlags(bool warn, bool lockout, bool tripped) {
  return (warn ? pc::espnow::StatusWarn : 0) | (lockout ? pc::espnow::StatusLockout : 0) |
         (tripped ? pc::espnow::StatusTripped : 0) | pc::espnow::StatusHasLimit;
}
}  // namespace aceplug
