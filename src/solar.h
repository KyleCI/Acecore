#pragma once
#include <Arduino.h>
// PV input via the ADS1115 on I2C (IO21 SDA / IO22 SCL), ported from the Dubem
// firmware (src/mppt.cpp): A0-A1 differential across a 2 mOhm shunt at
// GAIN_SIXTEEN, A2 single-ended on a 200k/3.3k divider at GAIN_ONE (about
// 250 V full scale), 128 SPS, 16-sample averages, published every 500 ms by a
// task on core 1. The part is fitted; if it does not answer, readings are null.
namespace solar {
struct Reading { bool ok = false; float voltage = 0, current = 0, power = 0; uint32_t ageMs = 0; };
void begin();
Reading read();   // ok=false if no reading in the last 5 s
bool present();
}  // namespace solar
