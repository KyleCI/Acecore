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

// Presence must hold for this long before the published flag flips.
// The poll task runs once per second, so two seconds is two good (or bad) samples.
constexpr uint32_t kPresentDebounceMs = 2000;

// Time debounce around presentHysteresis. Candidate is the hysteresis output.
// The published flag changes only after the candidate holds for kHoldMs.
struct PresentDebounce {
  static constexpr uint32_t kHoldMs = kPresentDebounceMs;
  bool present = false;
  bool pending = false;
  bool have_since = false;
  uint32_t since_ms = 0;

  bool update(bool candidate, uint32_t now_ms) {
    if (candidate == present) {
      pending = candidate;
      since_ms = now_ms;
      have_since = true;
      return present;
    }
    if (!have_since || pending != candidate) {
      pending = candidate;
      since_ms = now_ms;
      have_since = true;
      return present;
    }
    if (static_cast<uint32_t>(now_ms - since_ms) >= kHoldMs) {
      present = candidate;
      pending = candidate;
      since_ms = now_ms;
    }
    return present;
  }
};

// App-facing volts when the flag is off: 0. Callers should send JSON null
// (never a crosstalk reading such as 36 V or 88 V).
inline float publishedVoltage(bool present, float raw_v) {
  return present ? raw_v : 0.f;
}

// inverter_on does not, by itself, invent or hide a 230 V mains reading:
// real grid can be present while the inverter is on. A sense reading below
// the enter threshold is never grid, including while the inverter is on
// (GPIO39 can pick up inverter output as 36-88 V with no mains).
inline bool senseIsGrid(bool was_present, float raw_v, bool inverter_on) {
  (void)inverter_on;
  return presentHysteresis(was_present, raw_v);
}

}  // namespace grid
}  // namespace powercell

struct DspGridReading {
  bool valid = false;
  float vrms = 0;      // published volts: 0 when grid is not present
  float raw_vrms = 0;  // GPIO39 ADC reading, even when not present
  bool clipped = false;
  bool present = false;
  uint32_t age_ms = 0;
  bool calibrated = false;
};

void dspGridBegin();
bool dspGridRead(DspGridReading* out);
