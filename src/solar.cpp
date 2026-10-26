#include "solar.h"
#include <Adafruit_ADS1X15.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "log.h"
#include "pins.h"

namespace solar {
namespace {
constexpr float DIVIDER = (200000.0f + 3300.0f) / 3300.0f, VCAL = 1.0f, SHUNT_OHMS = 0.002f;
constexpr float LSB_SIXTEEN = 0.256f / 32768.0f, LSB_ONE = 4.096f / 32768.0f;
constexpr uint8_t SAMPLES = 16;
constexpr uint32_t PERIOD_MS = 500, STALE_MS = 5000;

Adafruit_ADS1115 ads;
SemaphoreHandle_t mx = nullptr;
Reading latest;
uint32_t at = 0;
bool g_present = false;

int32_t avgDiff01() {
  int32_t s = 0;
  for (uint8_t i = 0; i < SAMPLES; ++i) { s += ads.readADC_Differential_0_1(); vTaskDelay(pdMS_TO_TICKS(1)); }
  return s / SAMPLES;
}
int32_t avgSingle(uint8_t ch) {
  int32_t s = 0;
  for (uint8_t i = 0; i < SAMPLES; ++i) { s += ads.readADC_SingleEnded(ch); vTaskDelay(pdMS_TO_TICKS(1)); }
  return s / SAMPLES;
}

void task(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    ads.setGain(GAIN_SIXTEEN);
    float current = avgDiff01() * LSB_SIXTEEN / SHUNT_OHMS;
    ads.setGain(GAIN_ONE);
    float v = avgSingle(2) * LSB_ONE * DIVIDER * VCAL;
    xSemaphoreTake(mx, portMAX_DELAY);
    latest.ok = true; latest.voltage = v; latest.current = current; latest.power = v * current; at = millis();
    xSemaphoreGive(mx);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
  }
}
}  // namespace

void begin() {
  mx = xSemaphoreCreateMutex();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!ads.begin(0x48, &Wire)) { LOGE("SOLAR", "ADS1115", "not answering on I2C 0x48; PV readings will be null"); return; }
  ads.setDataRate(RATE_ADS1115_128SPS);
  g_present = true;
  xTaskCreatePinnedToCore(task, "solar", 4096, nullptr, 1, nullptr, 1);
  LOGI("SOLAR", "ADS1115", "ok, divider %.2f, shunt %.3f ohm", DIVIDER, SHUNT_OHMS);
}

bool present() { return g_present; }

Reading read() {
  Reading r;
  if (!mx) return r;
  xSemaphoreTake(mx, portMAX_DELAY);
  uint32_t age = millis() - at;
  if (latest.ok && age < STALE_MS) { r = latest; r.ageMs = age; }
  xSemaphoreGive(mx);
  return r;
}
}  // namespace solar
