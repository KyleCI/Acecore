#include "grid_sense.h"

#include <math.h>

namespace powercell {
namespace grid {

bool anyClipped(const float* pin_mv, size_t count) {
  if (!pin_mv || count == 0) return false;
  for (size_t i = 0; i < count; ++i) {
    if (pin_mv[i] > kClipHighMv || pin_mv[i] < kClipLowMv) return true;
  }
  return false;
}

float meanMv(const float* pin_mv, size_t count) {
  if (!pin_mv || count == 0) return 0.f;
  double sum = 0;
  for (size_t i = 0; i < count; ++i) sum += pin_mv[i];
  return static_cast<float>(sum / static_cast<double>(count));
}

WaveResult fromPinMillivolts(const float* pin_mv, size_t count, float k) {
  WaveResult r;
  if (!pin_mv || count < 4 || !(k > 0.f) || !isfinite(k)) return r;
  const float mean = meanMv(pin_mv, count);
  if (!isfinite(mean)) return r;
  double acc = 0;
  for (size_t i = 0; i < count; ++i) {
    const double dv = (static_cast<double>(pin_mv[i]) - mean) / 1000.0;
    acc += dv * dv;
  }
  const float rms = static_cast<float>(sqrt(acc / static_cast<double>(count)));
  r.ac_ref_v = mean / 1000.f;
  r.vrms = k * rms;
  r.clipped = anyClipped(pin_mv, count);
  r.ok = isfinite(r.vrms);
  return r;
}

bool presentHysteresis(bool was_present, float vrms) {
  if (!isfinite(vrms)) return was_present;
  if (was_present) return vrms >= kPresentExitV;
  return vrms >= kPresentEnterV;
}

}  // namespace grid
}  // namespace powercell

#if defined(ARDUINO)
#include "config.h"
#include <Arduino.h>

#if POWERCELL_DSP011
#include "pins.h"
#include "prefs_safe.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
#ifndef DSP_GRID_K
#define DSP_GRID_K 357.54
#endif
float g_k = static_cast<float>(DSP_GRID_K);
SemaphoreHandle_t g_mu = nullptr;
bool g_valid = false;
float g_vrms = 0;
bool g_clipped = false;
bool g_present = false;
uint32_t g_at = 0;
powercell::grid::PresentDebounce g_deb;

void loadScale() {
  g_k = static_cast<float>(DSP_GRID_K);
  Preferences p;
  if (!prefsOpenRO(p, "pc_cfg")) return;
  if (p.isKey("grid_k")) {
    const float k = p.getFloat("grid_k", g_k);
    if (k > 1.f && k < 10000.f) g_k = k;
  }
  p.end();
}

void capture(float* mv, int n) {
  const uint32_t t0 = micros();
  for (int i = 0; i < n; ++i) {
    const uint32_t target = t0 + static_cast<uint32_t>(i) * powercell::grid::kSampleIntervalUs;
    while (static_cast<int32_t>(micros() - target) < 0) {}
    mv[i] = static_cast<float>(analogReadMilliVolts(PIN_GRID_SENSE));
  }
}

void task(void*) {
  float mv[powercell::grid::kSampleCount];
  for (;;) {
    capture(mv, powercell::grid::kSampleCount);
    const powercell::grid::WaveResult wave =
        powercell::grid::fromPinMillivolts(mv, powercell::grid::kSampleCount, g_k);
    if (g_mu && xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (wave.ok) {
        const bool candidate = powercell::grid::presentHysteresis(g_deb.present, wave.vrms);
        g_present = g_deb.update(candidate, millis());
        g_vrms = wave.vrms;
        g_clipped = wave.clipped;
        g_valid = true;
        g_at = millis();
      }
      xSemaphoreGive(g_mu);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}  // namespace

void dspGridBegin() {
  loadScale();
  analogSetPinAttenuation(PIN_GRID_SENSE, ADC_11db);
  g_mu = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "grid", 4096, nullptr, 1, nullptr, 0);
}

bool dspGridRead(DspGridReading* out) {
  if (!out || !g_mu || xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  const uint32_t age = g_valid ? static_cast<uint32_t>(millis() - g_at) : 0;
  const bool fresh = g_valid && age < 5000;
  out->valid = fresh;
  out->raw_vrms = fresh ? g_vrms : 0;
  out->present = fresh && g_present;
  out->vrms = powercell::grid::publishedVoltage(out->present, out->raw_vrms);
  out->clipped = fresh && g_clipped;
  out->age_ms = fresh ? age : 0;
  out->calibrated = false;
  xSemaphoreGive(g_mu);
  return fresh;
}
#else
void dspGridBegin() {}

bool dspGridRead(DspGridReading* out) {
  (void)out;
  return false;
}
#endif
#endif
