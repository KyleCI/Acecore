#pragma once
// Status LEDs (BC337 drivers): green = all OK and cloud connected,
// yellow = offline / setup AP (blinks) / battery < 25 %, red = STM32 fault or
// battery < 10 %, amber = BMS alarm (steady) or BMS not answering (blink).
// PWR_LIGHT (IO25) is driven by gateway.cpp.
namespace leds { void begin(); void service(); }
