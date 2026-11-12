#pragma once
// DSP v0.1.1 battery status light (thunderbolt): three single-colour LEDs,
// one lit at a time, matching Dubem's working firmware (lighting.cpp/.h).
// Caleb confirmed the colour rules 2026-11-10 21:13 WAT, then on 2026-11-12
// swapped the orange and red pins (see pins.h): red GPIO13, orange GPIO12
// (PIN_LED_ORANGE, mid-SOC, schematic Y), green GPIO14.
//   SOC >= 70 %  GREEN     SOC 40..69 %  BLUE/orange     SOC < 40 %  RED
//   No BMS data, or older than 15 s: RED
//   Grid present: the lit LED breathes (4 s triangle). No grid: solid.
// Pure logic, host-tested. Pins and LEDC live in src/leds.cpp.
#include <stdint.h>

namespace powercell {

enum class StatusColor : uint8_t { Red = 0, Blue = 1, Green = 2 };

constexpr uint32_t kLedPwmFreqHz = 5000;
constexpr uint8_t kLedPwmBits = 8;
constexpr uint32_t kLedBreatheMs = 4000;
constexpr uint32_t kLedBmsStaleMs = 15000;
constexpr uint8_t kLedGreenSoc = 70;
constexpr uint8_t kLedBlueSoc = 40;

struct StatusLed {
  StatusColor color;
  uint8_t duty;  // 0..255 on the lit LED; the other two are 0
};

// 0 -> 255 -> 0 over kLedBreatheMs, as Dubem's breathe().
inline uint8_t led_breathe(uint32_t now_ms) {
  const uint32_t half = kLedBreatheMs / 2;
  const uint32_t p = now_ms % kLedBreatheMs;
  const uint32_t v = (p < half) ? p : (kLedBreatheMs - p);
  return static_cast<uint8_t>((v * 255UL) / half);
}

inline StatusColor led_color(bool bms_fresh, uint8_t soc) {
  if (!bms_fresh) return StatusColor::Red;
  if (soc >= kLedGreenSoc) return StatusColor::Green;
  if (soc >= kLedBlueSoc) return StatusColor::Blue;
  return StatusColor::Red;
}

// bms_fresh: a valid BMS frame no older than kLedBmsStaleMs.
inline StatusLed dsp_status_led(bool bms_fresh, uint8_t soc, bool grid_present, uint32_t now_ms) {
  return {led_color(bms_fresh, soc), grid_present ? led_breathe(now_ms) : static_cast<uint8_t>(255)};
}

}  // namespace powercell
