#pragma once

#include <stddef.h>
#include <stdint.h>

// DSP grid sense. The pin is the v0.2 divider: two 1 M legs plus 2.2 k in
// series (2.0022 M) into 5.6 k, gain 0.0027969. K = 1/gain = 357.54.
// Vgrid_rms = K * RMS(Vpin - mean(Vpin)). The mean is the AC reference.
// It is not assumed to be 1.65 V.
namespace powercell {
namespace grid {

constexpr float kScale = 357.54f;
constexpr float kClipHighMv = 2450.f;
constexpr float kClipLowMv = 150.f;
// Same window as the STM32 grid-present rule (events_core.h).
constexpr float kPresentEnterV = 170.f;
constexpr float kPresentExitV = 160.f;
constexpr int kCycles = 2;
constexpr int kSamplesPerCycle = 40;  // 2 kHz for a 50 Hz fundamental
constexpr int kSampleCount = kCycles * kSamplesPerCycle;  // 40 ms
constexpr uint32_t kSampleIntervalUs = 500;

struct WaveResult {
  float ac_ref_v = 0;
  float vrms = 0;
  bool clipped = false;
  bool ok = false;
};

bool anyClipped(const float* pin_mv, size_t count);
float meanMv(const float* pin_mv, size_t count);
// k is the scale. A non-positive k is rejected.
WaveResult fromPinMillivolts(const float* pin_mv, size_t count, float k);
// was_present uses the 170 V enter / 160 V exit window.
bool presentHysteresis(bool was_present, float vrms);

}  // namespace grid
}  // namespace powercell

struct DspGridReading {
  bool valid = false;
  float vrms = 0;
  bool clipped = false;
  bool present = false;
  uint32_t age_ms = 0;
  bool calibrated = false;
};

void dspGridBegin();
bool dspGridRead(DspGridReading* out);
