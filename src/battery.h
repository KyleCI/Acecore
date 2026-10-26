#pragma once
#include <Arduino.h>

// Pack-level battery state, merged from the owners agreed with hardware:
//   - JK-BMS (bms.h) owns SoC, cells, pack current, temperatures, cycles, alarms.
//     Its pack voltage is the one shown.
//   - STM32 owns its PA3 battery voltage (protection input). Kept as a
//     cross-check, and used for voltage and SoC only when the BMS is offline.
//   - IO34 BAT_SENSE (same buffer as PA3) is the last resort when both are silent.
// Low/critical alerts follow `soc`, so they come from the BMS when it is
// online and fall back to the STM32 otherwise.
namespace battery {
struct Reading {
  bool valid = false;        // voltage and soc below are meaningful
  float voltage = 0;         // shown pack voltage
  uint8_t soc = 0;
  const char *source = "none";      // voltage/soc source: "bms" | "stm32" | "adc" | "none"
  bool hasStm32V = false;  float stm32V = 0;  // STM32 PA3 reading (cross-check)
  bool hasAdcV = false;    float adcV = 0;    // ESP IO34 reading
  bool mismatch = false;     // BMS and STM32 voltages differ by > PACK_V_CROSSCHECK_TOL
  uint32_t ageMs = 0;
};
void begin();
Reading read();
// STM32 telemetry SoC: linear 46.0 V = 0 % .. 54.8 V = 100 % (same points as the STM32 firmware).
uint8_t socFromVoltage(float packV);
}  // namespace battery
