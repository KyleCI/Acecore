#include "hw_pzem.h"
#if POWERCELL_HW_V011
#include "pzem_v3.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
constexpr int kRx = 25;
constexpr int kTx = 26;
constexpr uint32_t kFreshMs = 2000;
HardwareSerial& port = Serial1;
SemaphoreHandle_t mu = nullptr;
bool valid = false;
float volts = 0, amps = 0, watts = 0;
uint32_t wh = 0, at_ms = 0;

bool publishable(float w) {
  return w == w && w > -0.5f && w < 90000.f && w != -1.f;
}

void poll() {
  powercell::PzemConfig cfg;
  cfg.address = 0xF8;
  cfg.timeout_ms = 200;
  uint8_t request[8];
  const size_t n = powercell::pzem_build_read(cfg, request, sizeof request);
  while (port.available()) port.read();
  if (n) port.write(request, n);
  uint8_t response[32];
  size_t got = 0;
  const uint32_t start = millis();
  while (got < 25 && millis() - start < cfg.timeout_ms) {
    if (port.available()) response[got++] = static_cast<uint8_t>(port.read());
    else vTaskDelay(1);
  }
  const powercell::PzemReading reading = powercell::pzem_parse(cfg, response, got, got < 25);
  uint32_t energy = 0;
  const bool ok = powercell::pzem_wh(reading, &energy);
  const float w = reading.power_dW / 10.0f;
  if (!ok || !publishable(w)) {
    if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(10)) == pdTRUE) {
      valid = false;
      xSemaphoreGive(mu);
    }
    return;
  }
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(10)) == pdTRUE) {
    valid = true;
    volts = reading.voltage_dV / 10.0f;
    amps = reading.current_mA / 1000.0f;
    watts = w;
    wh = energy;
    at_ms = millis();
    xSemaphoreGive(mu);
  }
}

void task(void*) {
  for (;;) {
    poll();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}  // namespace

void hwPzemBegin() {
  mu = xSemaphoreCreateMutex();
  port.begin(9600, SERIAL_8N1, kRx, kTx);
  xTaskCreatePinnedToCore(task, "pzem", 4096, nullptr, 1, nullptr, 1);
}

bool hwPzemSample(float* v, float* a, float* w, uint32_t* energy, uint32_t* age) {
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  const bool fresh = valid && static_cast<uint32_t>(millis() - at_ms) <= kFreshMs;
  if (fresh) {
    if (v) *v = volts;
    if (a) *a = amps;
    if (w) *w = watts;
    if (energy) *energy = wh;
    if (age) *age = millis() - at_ms;
  }
  xSemaphoreGive(mu);
  return fresh;
}
#endif
