#include "stm_energy.h"

#include <math.h>

namespace powercell {

bool StmEnergyEstimate::plausible(float volts, float amps) {
  if (!isfinite(volts) || !isfinite(amps)) return false;
  if (volts < 0.f || volts > kStmMaxVolts) return false;
  if (amps < 0.f || amps > kStmMaxAmps) return false;
  if (amps > kStmIdleAmps && volts < kStmMinLoadedVolts) return false;
  return true;
}

float StmEnergyEstimate::activeWatts(float volts, float amps) {
  return volts * amps * kStmPowerFactor;
}

void StmEnergyEstimate::restore(int32_t day_id, double today_wh, double lifetime_wh) {
  if (!isfinite(today_wh) || today_wh < 0) today_wh = 0;
  if (!isfinite(lifetime_wh) || lifetime_wh < 0) lifetime_wh = 0;
  if (today_wh > lifetime_wh) lifetime_wh = today_wh;
  day_id_ = day_id > 0 ? day_id : 0;
  today_wh_ = today_wh;
  lifetime_wh_ = lifetime_wh;
  committed_day_ = day_id_;
  committed_today_wh_ = today_wh_;
  committed_lifetime_wh_ = lifetime_wh_;
  have_anchor_ = false;
  last_unix_ = 0;
  ever_saved_ = day_id_ > 0 || lifetime_wh_ > 0 || today_wh_ > 0;
  dirty_ = false;
}

void StmEnergyEstimate::noteDay(int32_t day_id) {
  clock_day_ = day_id > 0 ? day_id : 0;
  if (clock_day_ == 0) return;
  if (day_id_ == clock_day_) return;
  day_id_ = clock_day_;
  today_wh_ = 0;
  dirty_ = true;
}

void StmEnergyEstimate::reanchor(const StmEstimateSample& sample) {
  have_anchor_ = true;
  last_mono_ = sample.mono_ms;
  last_unix_ = sample.unix_s > 0 ? sample.unix_s : 0;
}

StmEstimateView StmEnergyEstimate::makeView() const {
  StmEstimateView v;
  v.lifetime_wh = committed_lifetime_wh_;
  v.today_known = clock_day_ > 0 && committed_day_ == clock_day_;
  v.today_kwh = v.today_known ? committed_today_wh_ / 1000.0 : 0;
  v.persist_day_id = day_id_;
  v.persist_today_wh = today_wh_;
  v.persist_lifetime_wh = lifetime_wh_;
  v.persist = dirty_;
  if (dirty_ && ever_saved_) {
    const uint32_t elapsed = last_mono_ - last_save_mono_;
    const double unsaved = lifetime_wh_ - committed_lifetime_wh_;
    const bool due = (unsaved >= kStmSaveWh && elapsed >= kStmSaveMinMs) ||
                     (unsaved > kStmSaveIdleWh && elapsed >= kStmSaveMaxMs);
    const bool day_changed = day_id_ != committed_day_;
    if (!due && !day_changed) v.persist = false;
  }
  return v;
}

void StmEnergyEstimate::ackPersist(uint32_t mono_ms) {
  if (lifetime_wh_ + 1e-9 < committed_lifetime_wh_) return;
  committed_lifetime_wh_ = lifetime_wh_;
  committed_today_wh_ = today_wh_;
  committed_day_ = day_id_;
  last_save_mono_ = mono_ms;
  ever_saved_ = true;
  dirty_ = false;
}

StmEstimateView StmEnergyEstimate::observe(const StmEstimateSample& sample) {
  noteDay(sample.day_id);
  const bool usable = sample.valid && plausible(sample.volts, sample.amps);
  if (!usable) return makeView();
  if (!have_anchor_) {
    reanchor(sample);
    return makeView();
  }
  if (static_cast<int32_t>(sample.mono_ms - last_mono_) < 0) {
    reanchor(sample);
    return makeView();
  }
  const uint32_t dt_ms = sample.mono_ms - last_mono_;
  if (dt_ms > kStmMaxGapMs) {
    reanchor(sample);
    return makeView();
  }
  if (last_unix_ > 0 && sample.unix_s > 0) {
    const int64_t unix_delta = sample.unix_s - last_unix_;
    const int64_t mono_s = static_cast<int64_t>(dt_ms / 1000u);
    if (unix_delta < -1 || unix_delta > mono_s + kStmClockSlackS) {
      reanchor(sample);
      return makeView();
    }
  }
  const double wh = static_cast<double>(activeWatts(sample.volts, sample.amps)) * (static_cast<double>(dt_ms) / 3600000.0);
  if (wh > 0) {
    lifetime_wh_ += wh;
    if (clock_day_ > 0 && day_id_ == clock_day_) today_wh_ += wh;
    dirty_ = true;
  }
  reanchor(sample);
  return makeView();
}

}  // namespace powercell

#if defined(ARDUINO)
#include "clock.h"
#include "config.h"
#include "prefs_safe.h"
#include "solar_energy.h"
#include "stm32_link.h"
#include "time_policy.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/semphr.h>
#include <time.h>

namespace {
constexpr const char* kNs = "stm_wh";
powercell::StmEnergyEstimate g_est;
uint32_t g_seq = 0;
bool g_have_seq = false;
SemaphoreHandle_t g_mu = nullptr;
bool g_today_known = false;
double g_today_kwh = 0;
double g_lifetime_wh = 0;

void remember(const powercell::StmEstimateView& view) {
  g_today_known = view.today_known;
  g_today_kwh = view.today_kwh;
  g_lifetime_wh = view.lifetime_wh;
}

int32_t lagosDay() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  const pc::clk::Source src = clock_::source();
  const bool synced = now >= pc::clk::kMinEpoch &&
                      (src == pc::clk::Source::Ntp || src == pc::clk::Source::Phone || src == pc::clk::Source::Rtc);
  if (!synced) return 0;
  const int32_t day = pc::solar::localDay(now, true, powercell::kLagosOffsetMin);
  return day > 0 ? day : 0;
}

void fillClock(powercell::StmEstimateSample* sample) {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  sample->day_id = lagosDay();
  if (sample->day_id > 0) sample->unix_s = now;
}

bool writeEstimate(const powercell::StmEstimateView& view) {
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  p.putInt("day", view.persist_day_id);
  p.putDouble("today", view.persist_today_wh);
  p.putDouble("life", view.persist_lifetime_wh);
  p.end();
  return true;
}
}  // namespace

void stmEnergyBegin() {
#if POWERCELL_DSP011
  return;
#else
  g_mu = xSemaphoreCreateMutex();
  Preferences p;
  if (prefsOpenRO(p, kNs)) {
    const int32_t day = p.getInt("day", 0);
    const double today = p.getDouble("today", 0);
    const double life = p.getDouble("life", 0);
    p.end();
    g_est.restore(day, today, life);
  }
  powercell::StmEstimateSample sample;
  sample.mono_ms = millis();
  fillClock(&sample);
  remember(g_est.observe(sample));
#endif
}

void stmEnergyService() {
#if POWERCELL_DSP011
  return;
#else
  static uint32_t last_note = 0;
  pc::Stm32Status status;
  uint32_t age = 0;
  const bool have = stm32link::status(status, &age);
  const bool fresh = have && age < STM32_STALE_MS && status.has_vout && status.has_iout &&
                     (!g_have_seq || status.seq != g_seq);
  if (!fresh && last_note && millis() - last_note < 1000) return;
  last_note = millis();
  powercell::StmEstimateSample sample;
  sample.mono_ms = last_note;
  fillClock(&sample);
  if (fresh) {
    g_have_seq = true;
    g_seq = status.seq;
    sample.valid = powercell::StmEnergyEstimate::plausible(status.vout, status.iout);
    sample.volts = status.vout;
    sample.amps = status.iout;
  }
  if (!g_mu || xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) != pdTRUE) return;
  powercell::StmEstimateView view = g_est.observe(sample);
  xSemaphoreGive(g_mu);
  if (view.persist && writeEstimate(view)) {
    if (xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
      g_est.ackPersist(sample.mono_ms);
      view = g_est.publish();
      xSemaphoreGive(g_mu);
    }
  }
  if (g_mu && xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
    remember(view.persist ? g_est.publish() : view);
    xSemaphoreGive(g_mu);
  }
#endif
}

bool stmEnergyTodayKwh(double* kwh) {
#if POWERCELL_DSP011
  (void)kwh;
  return false;
#else
  if (!kwh || !g_mu || xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  const bool known = g_today_known;
  if (known) *kwh = g_today_kwh;
  xSemaphoreGive(g_mu);
  return known;
#endif
}

double stmEnergyLifetimeWh() {
#if POWERCELL_DSP011
  return 0;
#else
  if (!g_mu || xSemaphoreTake(g_mu, pdMS_TO_TICKS(20)) != pdTRUE) return g_lifetime_wh;
  const double wh = g_lifetime_wh;
  xSemaphoreGive(g_mu);
  return wh;
#endif
}
#endif
