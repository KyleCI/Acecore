#pragma once
#include "config.h"
#include <stdint.h>

#if POWERCELL_HW_V011
void hwPzemBegin();
// Fresh PZEM-004T V3 100 A sample on UART1 RX25/TX26.
// Power is active watts. Energy is lifetime watt-hours.
// A missing, stale, or sentinel reading returns false and does not invent zero.
bool hwPzemSample(float* volts, float* amps, float* active_w, uint32_t* lifetime_wh, uint32_t* age_ms);
#endif
