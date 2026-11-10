#pragma once
#include "config.h"
#include <stdint.h>

#if POWERCELL_DSP011
void hwPzemBegin();
// Fresh PZEM-004T V3 sample on UART1 RX GPIO35 (board pull-up to 3.3 V) / TX GPIO32, 9600 8N1.
// Power is active watts. Energy is lifetime watt-hours.
// A missing, stale, or sentinel reading returns false and does not invent zero.
bool hwPzemSample(float* volts, float* amps, float* active_w, uint32_t* lifetime_wh, uint32_t* age_ms);
// Today's AC output energy in kWh from the PZEM lifetime counter.
// False means the value is unknown: omit it, do not send 0.
bool hwAcEnergyTodayKwh(double* kwh);
// Poll counters for SELFTEST. False before hwPzemBegin().
struct HwPzemStats { uint32_t polls, ok, no_reply, short_frames, bad_frames, consecutive_fail; bool ever_ok; uint32_t last_ok_age_ms; };
bool hwPzemStats(HwPzemStats* out);
#endif
