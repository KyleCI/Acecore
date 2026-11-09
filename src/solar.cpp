#include "solar.h"
#include <Wire.h>
#include <time.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "live.h"
#include "log.h"
#include "pins.h"
#include <Preferences.h>
#include "clock.h"
#include "devauth.h"
#include "prefs_safe.h"
#include "solar_energy.h"

// 2.5.4: direct, bounded ADS1115 single-shot reads (no Adafruit busy-wait on
// conversionComplete, which never ends if the bus stops answering). Every I2C
// transfer has a 20 ms Wire timeout; a cycle aborts on the first error. After
// 3 consecutive failed cycles the task backs off 1 s .. 30 s (pc::live::Backoff),
// readings go invalid at once (null in JSON, never stale numbers), one WARN when
// it goes bad and one INFO when it recovers. The task runs on its own and only
// holds the reading mutex for a copy, so an I2C stall cannot block loop().
namespace solar {
namespace {
constexpr float DIVIDER = (200000.0f + 3300.0f) / 3300.0f, VCAL = 1.0f, SHUNT_OHMS = 0.002f;
constexpr float LSB_SIXTEEN = 0.256f / 32768.0f, LSB_ONE = 4.096f / 32768.0f;
constexpr uint8_t ADDR = 0x48, SAMPLES = 16;
constexpr uint32_t PERIOD_MS = 500, STALE_MS = 5000, I2C_TIMEOUT_MS = 20;
// Config register: OS=1 single shot, MODE=1, DR=128 SPS (100), comparator off (11)
constexpr uint16_t CFG_BASE = 0x8000 | 0x0100 | 0x0080 | 0x0003;
constexpr uint16_t MUX_DIFF_0_1 = 0x0000, MUX_SINGLE_2 = 0x6000;
constexpr uint16_t PGA_4V096 = 0x0200, PGA_0V256 = 0x0A00;

SemaphoreHandle_t mx = nullptr;
Reading latest;
uint32_t at = 0;
bool g_present = false;
pc::live::Backoff g_bo;
uint32_t g_errors = 0;
bool g_everGood = false;
pc::solar::Energy g_energy;  // 2.5.8, guarded by mx
const char *NS = "solar";

int32_t dayNow() { return pc::solar::localDay((int64_t)time(nullptr), devauth::clockTrusted(), clock_::tzOffsetMin()); }

void energyStep(bool ok, float powerW) {
  int32_t day = dayNow();
  uint32_t now = millis();
  xSemaphoreTake(mx, portMAX_DELAY);
  g_energy.update(ok, powerW, now, day);
  bool due = g_energy.saveDue(now);
  double tot = g_energy.totalWh(), tdy = g_energy.todayWh(); int32_t d = g_energy.day();
  if (due) g_energy.markSaved(now);
  xSemaphoreGive(mx);
  if (due) {
    Preferences p;
    if (p.begin(NS, false)) { p.putDouble("tot_wh", tot); p.putDouble("tdy_wh", tdy); p.putInt("day", d); p.end(); }
  }
}

bool writeReg(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg); Wire.write(uint8_t(v >> 8)); Wire.write(uint8_t(v));
  return Wire.endTransmission() == 0;
}
bool readReg(uint8_t reg, uint16_t &v) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(ADDR, (uint8_t)2) != 2) return false;
  v = uint16_t(Wire.read()) << 8; v |= uint8_t(Wire.read());
  return true;
}
// One conversion: start, wait (128 SPS = 7.8 ms), poll OS bit at most 3 times.
bool convert(uint16_t mux, uint16_t pga, int16_t &out) {
  if (!writeReg(0x01, CFG_BASE | mux | pga)) return false;
  vTaskDelay(pdMS_TO_TICKS(9));
  uint16_t cfg = 0;
  for (int i = 0; i < 3; ++i) {
    if (!readReg(0x01, cfg)) return false;
    if (cfg & 0x8000) break;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (!(cfg & 0x8000)) return false;
  uint16_t raw;
  if (!readReg(0x00, raw)) return false;
  out = int16_t(raw);
  return true;
}
bool average(uint16_t mux, uint16_t pga, int32_t &avg) {
  int32_t s = 0;
  for (uint8_t i = 0; i < SAMPLES; ++i) { int16_t v; if (!convert(mux, pga, v)) return false; s += v; }
  avg = s / SAMPLES;
  return true;
}

void markInvalid() {
  xSemaphoreTake(mx, portMAX_DELAY); latest.ok = false; xSemaphoreGive(mx);
}

void task(void *) {
  for (;;) {
    uint32_t now = millis();
    if (!g_bo.due(now)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    int32_t ci = 0, vi = 0;
    bool ok = average(MUX_DIFF_0_1, PGA_0V256, ci) && average(MUX_SINGLE_2, PGA_4V096, vi);
    if (ok) {
      float current = ci * LSB_SIXTEEN / SHUNT_OHMS;
      float v = vi * LSB_ONE * DIVIDER * VCAL;
      xSemaphoreTake(mx, portMAX_DELAY);
      latest.ok = true; latest.voltage = v; latest.current = current; latest.power = v * current; at = millis(); g_everGood = true;
      xSemaphoreGive(mx);
      energyStep(true, v * current);
      if (g_bo.ok()) LOGI("SOLAR", "RECOVERED", "ADS1115 answering again after %lu I2C errors", (unsigned long)g_errors);
    } else {
      ++g_errors;
      markInvalid();
      energyStep(false, 0);
      if (g_bo.fail(millis()))
        LOGW("SOLAR", "I2C_FAIL", "ADS1115 not answering (%lu fails in a row); PV null, retry backs off to %lus",
             (unsigned long)g_bo.consecutive(), (unsigned long)(pc::live::Backoff::kMaxMs / 1000));
    }
    vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
  }
}
}  // namespace

void begin() {
  mx = xSemaphoreCreateMutex();
  {  // 2.5.8 restore the PV energy counters
    Preferences p;
    if (prefsOpenRO(p, NS)) {
      g_energy.restore(p.getDouble("tot_wh", 0), p.getDouble("tdy_wh", 0), p.getInt("day", pc::solar::kNoDay));
      p.end();
    }
  }
  // Arduino-ESP32 Wire logs every failed transfer ("[E][Wire.cpp:513] requestFrom(): i2cRead
  // returned Error 263"). Quiet the IDF tags where the core routes log_e through esp_log;
  // the back-off bounds the rest to one attempt per 30 s while the bus is bad.
  esp_log_level_set("Wire", ESP_LOG_NONE);
  esp_log_level_set("i2c", ESP_LOG_NONE);
  esp_log_level_set("i2c.master", ESP_LOG_NONE);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  Wire.beginTransmission(ADDR);
  if (Wire.endTransmission() != 0) { LOGE("SOLAR", "ADS1115", "not answering on I2C 0x48; PV readings will be null"); return; }
  g_present = true;
  xTaskCreatePinnedToCore(task, "solar", 4096, nullptr, 1, nullptr, 1);
  LOGI("SOLAR", "ADS1115", "ok, divider %.2f, shunt %.3f ohm, i2c timeout %lu ms", DIVIDER, SHUNT_OHMS, (unsigned long)I2C_TIMEOUT_MS);
}

bool present() { return g_present; }
bool failing() { return g_bo.failed(); }
uint32_t errors() { return g_errors; }
bool energy(double &todayKwh, double &totalKwh) {
  if (!mx || !g_present) return false;
  xSemaphoreTake(mx, portMAX_DELAY);
  todayKwh = g_energy.todayWh() / 1000.0; totalKwh = g_energy.totalWh() / 1000.0;
  xSemaphoreGive(mx);
  return true;
}
uint32_t lastGoodAgeMs() {
  if (!mx) return UINT32_MAX;
  xSemaphoreTake(mx, portMAX_DELAY);
  uint32_t r = g_everGood ? millis() - at : UINT32_MAX;
  xSemaphoreGive(mx);
  return r;
}

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
