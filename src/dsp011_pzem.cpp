#include "dsp011_pzem.h"
#if POWERCELL_DSP011
#include "ac_energy_today.h"
#include "clock.h"
#include "prefs_safe.h"
#include "pzem_v3.h"
#include "solar_energy.h"
#include "time_policy.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace {
constexpr int kRx = 25;
constexpr int kTx = 26;
constexpr uint32_t kFreshMs = 2000;
constexpr const char* kTodayNs = "ac_today";
HardwareSerial& port = Serial1;
SemaphoreHandle_t mu = nullptr;
bool valid = false;
float volts = 0, amps = 0, watts = 0;
uint32_t wh = 0, at_ms = 0;
powercell::AcEnergyToday today;
bool today_known = false;
double today_kwh = 0;

void loadToday() {
  Preferences p;
  if (!prefsOpenRO(p, kTodayNs)) {
    today.restore(false, 0, 0, false);
    return;
  }
  const bool have = p.isKey("have") ? p.getUChar("have", 0) != 0 : p.isKey("base");
  today.restore(have, p.getInt("day", 0), p.getUInt("base", 0), p.getUChar("unk", 0) != 0);
  p.end();
}

void saveToday() {
  Preferences p;
  if (!p.begin(kTodayNs, false)) return;
  p.putUChar("have", today.baselineValid() ? 1 : 0);
  p.putInt("day", today.dayId());
  p.putUInt("base", today.baselineWh());
  p.putUChar("unk", today.dayUnknown() ? 1 : 0);
  p.end();
}

powercell::TodayInput todayInput(bool sample_ok, uint32_t lifetime_wh) {
  powercell::TodayInput in;
  in.valid = sample_ok;
  in.lifetime_wh = lifetime_wh;
  in.mono_ms = millis();
  in.mono_continuous = true;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  const pc::clk::Source src = clock_::source();
  const bool synced = now >= pc::clk::kMinEpoch &&
                      (src == pc::clk::Source::Ntp || src == pc::clk::Source::Phone || src == pc::clk::Source::Rtc);
  if (!synced) return in;
  const int32_t day = pc::solar::localDay(now, true, clock_::tzOffsetMin());
  if (day <= 0) return in;
  in.day_id = day;
  in.clock = src == pc::clk::Source::Rtc ? powercell::TodayClock::Rtc : powercell::TodayClock::Anchored;
  return in;
}

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
  const bool sample_ok = ok && publishable(w);
  const powercell::TodayInput day = todayInput(sample_ok, energy);
  bool write_baseline = false;
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!sample_ok) {
      valid = false;
    } else {
      valid = true;
      volts = reading.voltage_dV / 10.0f;
      amps = reading.current_mA / 1000.0f;
      watts = w;
      wh = energy;
      at_ms = millis();
    }
    const powercell::TodayOutput out = today.observe(day);
    today_known = today.known();
    today_kwh = today.kwh();
    write_baseline = out.persist;
    xSemaphoreGive(mu);
  }
  if (write_baseline) saveToday();
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
  loadToday();
  port.begin(9600, SERIAL_8N1, kRx, kTx);
  xTaskCreatePinnedToCore(task, "pzem", 6144, nullptr, 1, nullptr, 1);
}

bool hwAcEnergyTodayKwh(double* kwh) {
  if (!kwh || !mu || xSemaphoreTake(mu, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  const bool known = today_known;
  if (known) *kwh = today_kwh;
  xSemaphoreGive(mu);
  return known;
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
