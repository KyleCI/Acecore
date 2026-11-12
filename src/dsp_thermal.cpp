#include "dsp_thermal.h"

#include <math.h>

namespace powercell {
namespace thermal {

bool mvToCelsius(float mv, float *c, bool ntc_low_side) {
  if (!c || !isfinite(mv)) return false;
  if (mv < kFaultMv || mv > kVccMv - kFaultMv) return false;
  const float r = ntc_low_side ? kRFixedOhm * mv / (kVccMv - mv) : kRFixedOhm * (kVccMv - mv) / mv;
  if (!(r > 0.f) || !isfinite(r)) return false;
  const float inv_t = 1.0f / 298.15f + logf(r / kR25Ohm) / kBeta;
  if (!(inv_t > 0.f)) return false;
  const float t = 1.0f / inv_t - 273.15f;
  if (!(t >= kMinC && t <= kMaxC)) return false;
  *c = t;
  return true;
}

bool fanWanted(bool fan_on, bool fault, float c) {
  if (fault || !isfinite(c)) return true;
  if (c >= kFanOnC) return true;
  if (c <= kFanOffC) return false;
  return fan_on;
}

}  // namespace thermal
}  // namespace powercell

#if defined(ARDUINO)
#include "config.h"
#include <Arduino.h>

#if POWERCELL_DSP011
#include "log.h"
#include "pins.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t g_mu = nullptr;
DspThermalReading g_r;
uint32_t g_at = 0;

void setFan(bool on) { digitalWrite(PIN_FAN, on ? HIGH : LOW); }  // HIGH = ON

float readMv() {
  uint32_t sum = 0;
  for (int i = 0; i < powercell::thermal::kSamples; ++i) sum += analogReadMilliVolts(PIN_NTC);
  return sum / static_cast<float>(powercell::thermal::kSamples);
}

void task(void *) {
  bool fan = false, lastFault = false, first = true;
  for (;;) {
    const float mv = readMv();
    float c = NAN;
    const bool ok = powercell::thermal::mvToCelsius(mv, &c);
    const bool fault = !ok;
    const bool want = powercell::thermal::fanWanted(fan, fault, c);
    if (want != fan || first) {
      fan = want;
      setFan(fan);
      if (fault) LOGW("THERMAL", "FAN", "ON: sensor fault (fail-safe)");
      else LOGI("THERMAL", "FAN", "%s at %.1f C", fan ? "ON" : "OFF", c);
    }
    if (fault && !lastFault) LOGW("THERMAL", "NTC_FAULT", "GPIO%d %.0f mV: open, short or out of range", PIN_NTC, mv);
    lastFault = fault;
    first = false;
    if (g_mu && xSemaphoreTake(g_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
      g_r.sampled = true;
      g_r.fault = fault;
      g_r.valid = ok;
      g_r.temp_c = ok ? c : 0;
      g_r.mv = mv;
      g_r.fan_on = fan;
      g_at = millis();
      xSemaphoreGive(g_mu);
    }
#ifdef THERMAL_DEBUG
    Serial.printf("[Thermal] %.0f mV  %.1f C  fan %s%s\n", mv, ok ? c : NAN, fan ? "ON" : "off", fault ? "  FAULT" : "");
#endif
    vTaskDelay(pdMS_TO_TICKS(powercell::thermal::kPeriodMs));
  }
}
}  // namespace

void dspThermalBegin() {
  g_mu = xSemaphoreCreateMutex();
  digitalWrite(PIN_FAN, LOW);
  pinMode(PIN_FAN, OUTPUT);
  setFan(false);
  analogSetPinAttenuation(PIN_NTC, ADC_11db);
  xTaskCreatePinnedToCore(task, "thermal", 3072, nullptr, 1, nullptr, 1);
  LOGI("THERMAL", "BEGIN", "NTC 10k B3950 2k7 on GPIO%d, fan GPIO%d HIGH=ON (ON >= %.0f C, OFF <= %.0f C)", PIN_NTC, PIN_FAN,
       powercell::thermal::kFanOnC, powercell::thermal::kFanOffC);
}

bool dspThermalRead(DspThermalReading *out) {
  if (!out || !g_mu || xSemaphoreTake(g_mu, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  *out = g_r;
  const uint32_t age = g_r.sampled ? static_cast<uint32_t>(millis() - g_at) : 0;
  out->age_ms = age;
  const bool fresh = g_r.sampled && age < powercell::thermal::kStaleMs;
  out->valid = fresh && g_r.valid;
  if (!out->valid) out->temp_c = 0;
  xSemaphoreGive(g_mu);
  return out->valid;
}
#else
void dspThermalBegin() {}
bool dspThermalRead(DspThermalReading *out) {
  if (out) *out = DspThermalReading{};
  return false;
}
#endif
#endif
