#include "battery.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "bms.h"
#include "config.h"
#include "log.h"
#include "pins.h"
#include "stm32_link.h"

namespace battery {
namespace {
// IO34 sees the LM324 buffer output that feeds STM32 PA3. The STM32 scales PA3
// with DCBUS_ADC_SCALE = 0.01542 V/count (12-bit, 3.3 V), i.e. about 19.13 V
// of pack per volt at the pin. Calibrate per board revision on the bench.
constexpr float PACK_PER_PIN_V = 0.01542f * 4095.0f / 3.3f, EMA = 0.15f;
constexpr float PRESENT_MIN_V = 10.0f;  // STM32 VBAT_SENSE_MIN_V
// IO34 plausibility window for a 16S LiFePO4 pack; a floating pin (bare board) is ignored.
constexpr float ADC_MIN_V = 30.0f, ADC_MAX_V = 62.0f;
constexpr float SOC_0_V = 46.0f, SOC_100_V = 54.8f;

SemaphoreHandle_t mx = nullptr;
[[maybe_unused]] float ema = -1;
[[maybe_unused]] uint32_t at = 0;

#if FEATURE_BAT_SENSE_ADC
void task(void *) {
  analogSetPinAttenuation(PIN_BAT_SENSE, ADC_11db);
  for (;;) {
    uint32_t mv = 0;
    for (int i = 0; i < 64; ++i) mv += analogReadMilliVolts(PIN_BAT_SENSE);  // eFuse-calibrated
    float v = (mv / 64.0f / 1000.0f) * PACK_PER_PIN_V;
    xSemaphoreTake(mx, portMAX_DELAY);
    if (v < ADC_MIN_V || v > ADC_MAX_V) ema = -1;
    else { ema = ema < 0 ? v : ema + EMA * (v - ema); at = millis(); }
    xSemaphoreGive(mx);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
#endif
}  // namespace

uint8_t socFromVoltage(float v) {
  if (v <= SOC_0_V) return 0;
  if (v >= SOC_100_V) return 100;
  return static_cast<uint8_t>((v - SOC_0_V) / (SOC_100_V - SOC_0_V) * 100.0f + 0.5f);
}

void begin() {
  mx = xSemaphoreCreateMutex();
#if FEATURE_BAT_SENSE_ADC
  xTaskCreatePinnedToCore(task, "batsense", 3072, nullptr, 1, nullptr, 1);
#endif
}

Reading read() {
  Reading r;
  pc::Stm32Status s;
  uint32_t sAge = 0;
  if (stm32link::status(s, &sAge) && sAge < STM32_STALE_MS && s.has_bat && s.bat_v > PRESENT_MIN_V) {
    r.hasStm32V = true; r.stm32V = s.bat_v;
  }
#if FEATURE_BAT_SENSE_ADC
  uint32_t aAge = 0;
  if (mx) {
    xSemaphoreTake(mx, portMAX_DELAY);
    if (ema > 0) { r.hasAdcV = true; r.adcV = ema; aAge = millis() - at; }
    xSemaphoreGive(mx);
  }
#endif
  pc::jk::Data b;
  uint32_t bAge = 0;
  if (bms::read(b, &bAge)) {
    r.valid = true; r.voltage = b.packMv / 1000.0f; r.soc = b.soc; r.source = "bms"; r.ageMs = bAge;
    r.mismatch = r.hasStm32V && fabsf(r.stm32V - r.voltage) > PACK_V_CROSSCHECK_TOL;
  } else if (r.hasStm32V) {
    r.valid = true; r.voltage = r.stm32V; r.soc = s.has_bat_pct ? s.bat_pct : socFromVoltage(r.stm32V);
    r.source = "stm32"; r.ageMs = sAge;
  }
#if FEATURE_BAT_SENSE_ADC
  else if (r.hasAdcV) {
    r.valid = true; r.voltage = r.adcV; r.soc = socFromVoltage(r.adcV); r.source = "adc"; r.ageMs = aAge;
  }
#endif
  return r;
}
}  // namespace battery
