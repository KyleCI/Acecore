#include "dsp011_pzem.h"
#if POWERCELL_DSP011
#include "ac_energy_today.h"
#include "clock.h"
#include "prefs_safe.h"
#include "pzem_v3.h"
#include "pzem_stats.h"
#include "pzem_route.h"
#include "log.h"
#include "solar_energy.h"
#include "time_policy.h"
#include "pins.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_reg.h"
#include "soc/rtc_io_periph.h"

namespace {
constexpr int kRx = PIN_PZEM_RX;
constexpr int kTx = PIN_PZEM_TX;
constexpr uint32_t kFreshMs = 2000;
constexpr const char* kTodayNs = "ac_today";
HardwareSerial& port = Serial1;
SemaphoreHandle_t mu = nullptr;
SemaphoreHandle_t bus = nullptr;  // one UART1 transaction at a time (poll task, PZEM RAW)
uint16_t route_fault = 0;         // last pin-routing fault mask (0 = routed)
uint32_t route_heals = 0;
bool valid = false;
float volts = 0, amps = 0, watts = 0;
uint32_t wh = 0, at_ms = 0;
powercell::AcEnergyToday today;
bool today_known = false;
double today_kwh = 0;
powercell::PzemStats stats;

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
  if (!synced) {
    // Offline / no trusted clock: still start today from the first good PZEM read.
    in.day_id = powercell::kSessionDayId;
    in.clock = powercell::TodayClock::Session;
    return in;
  }
  const int32_t day = pc::solar::localDay(now, true, clock_::tzOffsetMin());
  if (day <= 0) {
    in.day_id = powercell::kSessionDayId;
    in.clock = powercell::TodayClock::Session;
    return in;
  }
  in.day_id = day;
  in.clock = src == pc::clk::Source::Rtc ? powercell::TodayClock::Rtc : powercell::TodayClock::Anchored;
  return in;
}

bool publishable(float w) {
  return w == w && w > -0.5f && w < 90000.f && w != -1.f;
}

// Plain volatile load. REG_READ's compile-time address check does not accept the
// computed addresses below.
uint32_t rd(uint32_t addr) { return *reinterpret_cast<volatile uint32_t*>(addr); }

bool rtcOwned(int gpio) {
  if (gpio < 0 || gpio >= SOC_GPIO_PIN_COUNT) return false;
  const int n = rtc_io_num_map[gpio];
  if (n < 0) return false;
  const rtc_io_desc_t& d = rtc_io_desc[n];
  return d.reg && (rd(d.reg) & d.mux);
}

powercell::PzemRouteRegs readRoute() {
  powercell::PzemRouteRegs r;
  r.driver = uart_is_driver_installed(UART_NUM_1);
  r.rx_in_sel = rd(GPIO_FUNC0_IN_SEL_CFG_REG + 4 * powercell::kU1RxdInIdx);
  r.tx_out_sel = rd(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * kTx);
  r.rx_iomux = rd(GPIO_PIN_MUX_REG[kRx]);
  r.tx_iomux = rd(GPIO_PIN_MUX_REG[kTx]);
  r.rx_rtc = rtcOwned(kRx);
  r.tx_rtc = rtcOwned(kTx);
  return r;
}

void logRoute(const char* event, const powercell::PzemRouteRegs& r, uint16_t f) {
  char names[96];
  powercell::pzem_route_names(f, names, sizeof names);
  const bool warn = f != 0;
  if (warn)
    LOGW("PZEM", event, "route=%s drv=%d in_sel=0x%03lX out_sel32=0x%03lX mux35=0x%04lX mux32=0x%04lX rtc35=%d rtc32=%d",
         names, (int)r.driver, (unsigned long)r.rx_in_sel, (unsigned long)r.tx_out_sel, (unsigned long)r.rx_iomux,
         (unsigned long)r.tx_iomux, (int)r.rx_rtc, (int)r.tx_rtc);
  else
    LOGI("PZEM", event, "route=ok drv=1 in_sel=0x%03lX out_sel32=0x%03lX mux35=0x%04lX mux32=0x%04lX",
         (unsigned long)r.rx_in_sel, (unsigned long)r.tx_out_sel, (unsigned long)r.rx_iomux, (unsigned long)r.tx_iomux);
}

// If anything took GPIO35/GPIO32 away from UART1 after begin(), hand them back.
void ensureRoute() {
  const powercell::PzemRouteRegs r = readRoute();
  const uint16_t f = powercell::pzem_route_check(r, kRx);
  if (f == route_fault) return;
  if (!f) { route_fault = 0; logRoute("ROUTE_OK", r, 0); return; }
  route_fault = f;
  logRoute("ROUTE_LOST", r, f);
  if (!r.driver) port.begin(9600, SERIAL_8N1, kRx, kTx);
  if (r.rx_rtc) rtc_gpio_deinit(static_cast<gpio_num_t>(kRx));
  if (r.tx_rtc) rtc_gpio_deinit(static_cast<gpio_num_t>(kTx));
  uart_set_pin(UART_NUM_1, kTx, kRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  ++route_heals;
  const powercell::PzemRouteRegs again = readRoute();
  const uint16_t g = powercell::pzem_route_check(again, kRx);
  logRoute(g ? "ROUTE_STILL_BAD" : "ROUTE_RESTORED", again, g);
  route_fault = g;
}

void poll() {
  if (bus && xSemaphoreTake(bus, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  ensureRoute();
  powercell::PzemConfig cfg;
  cfg.address = 0xF8;
  cfg.timeout_ms = 200;
  uint8_t request[8];
  const size_t n = powercell::pzem_build_read(cfg, request, sizeof request);
  while (port.available()) port.read();
  if (n) {
    port.write(request, n);
    port.flush();  // as Dubem pzem.cpp: start the reply timeout after the request is on the wire
  }
  uint8_t response[32];
  size_t got = 0;
  const uint32_t start = millis();
  while (got < 25 && millis() - start < cfg.timeout_ms) {
    if (port.available()) response[got++] = static_cast<uint8_t>(port.read());
    else vTaskDelay(1);
  }
  if (bus) xSemaphoreGive(bus);
  const powercell::PzemReading reading = powercell::pzem_parse(cfg, response, got, got < 25);
  uint32_t energy = 0;
  const bool ok = powercell::pzem_wh(reading, &energy);
  const float w = reading.power_dW / 10.0f;
  const bool sample_ok = ok && publishable(w);
  const powercell::TodayInput day = todayInput(sample_ok, energy);
  bool write_baseline = false;
  powercell::PzemStats::Log log_kind = powercell::PzemStats::Log::None;
  powercell::PzemPollOutcome log_outcome = powercell::PzemPollOutcome::Ok;
  powercell::PzemStats log_stats;
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
    const powercell::PzemPollOutcome outcome = powercell::pzem_classify(got, reading.status, sample_ok);
    log_kind = stats.record(outcome, millis());
    log_outcome = outcome;
    log_stats = stats;
    const powercell::TodayOutput out = today.observe(day);
    today_known = today.known();
    today_kwh = today.kwh();
    write_baseline = out.persist;
    xSemaphoreGive(mu);
  }
  if (write_baseline) saveToday();
  if (log_kind == powercell::PzemStats::Log::Warn)
    LOGW("PZEM", "NO_DATA", "%s rx=%d tx=%d baud=9600 polls=%lu ok=%lu no_reply=%lu short=%lu bad=%lu fail_run=%lu%s",
         powercell::pzem_outcome_name(log_outcome), kRx, kTx, (unsigned long)log_stats.polls, (unsigned long)log_stats.ok,
         (unsigned long)log_stats.no_reply, (unsigned long)log_stats.short_frames, (unsigned long)log_stats.bad_frames,
         (unsigned long)log_stats.consecutive_fail,
         log_outcome == powercell::PzemPollOutcome::NoReply ? " (check PZEM 5V, TX->GPIO35 pull-up to 3.3V, RX<-GPIO32)" : "");
  else if (log_kind == powercell::PzemStats::Log::Recovered)
    LOGI("PZEM", "RECOVERED", "polls=%lu ok=%lu", (unsigned long)log_stats.polls, (unsigned long)log_stats.ok);
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
  bus = xSemaphoreCreateMutex();
  loadToday();
  port.begin(9600, SERIAL_8N1, kRx, kTx);
  LOGI("PZEM", "INIT", "UART1 9600 8N1 rx=%d tx=%d addr=0xF8", kRx, kTx);
  {
    const powercell::PzemRouteRegs r = readRoute();
    logRoute("ROUTE", r, powercell::pzem_route_check(r, kRx));  // the first poll repairs a fault
  }
  xTaskCreatePinnedToCore(task, "pzem", 6144, nullptr, 1, nullptr, 1);
}

bool hwPzemStats(HwPzemStats* out) {
  if (!out || !mu || xSemaphoreTake(mu, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  out->polls = stats.polls;
  out->ok = stats.ok;
  out->no_reply = stats.no_reply;
  out->short_frames = stats.short_frames;
  out->bad_frames = stats.bad_frames;
  out->consecutive_fail = stats.consecutive_fail;
  out->ever_ok = stats.ever_ok;
  out->last_ok_age_ms = stats.ever_ok ? millis() - stats.last_ok_ms : UINT32_MAX;
  out->route_fault = route_fault;
  out->route_heals = route_heals;
  xSemaphoreGive(mu);
  return true;
}

// Bench diagnosis, read-only: one read-input-registers request (F8 04 0000 000A),
// then every byte that arrives in window_ms, hex-dumped. Never sends a reset.
bool hwPzemRaw(Print& out, uint32_t window_ms) {
  if (!bus) { out.println("PZEM RAW not started"); return false; }
  if (xSemaphoreTake(bus, pdMS_TO_TICKS(1500)) != pdTRUE) { out.println("PZEM RAW busy"); return false; }
  const powercell::PzemRouteRegs r = readRoute();
  const uint16_t f = powercell::pzem_route_check(r, kRx);
  char names[96];
  powercell::pzem_route_names(f, names, sizeof names);
  out.printf("PZEM RAW uart1 rx=%d tx=%d 9600 8N1 drv=%d route=%s in_sel=0x%03lX out_sel32=0x%03lX mux35=0x%04lX mux32=0x%04lX rtc35=%d rtc32=%d rx_level=%d\n",
             kRx, kTx, (int)r.driver, names, (unsigned long)r.rx_in_sel, (unsigned long)r.tx_out_sel,
             (unsigned long)r.rx_iomux, (unsigned long)r.tx_iomux, (int)r.rx_rtc, (int)r.tx_rtc, gpio_get_level(static_cast<gpio_num_t>(kRx)));
  powercell::PzemConfig cfg;
  uint8_t req[8];
  const size_t n = powercell::pzem_build_read(cfg, req, sizeof req);
  char hex[3 * 64 + 4];
  powercell::pzem_hex(req, n, hex, sizeof hex);
  size_t stale = 0;
  while (port.available()) { port.read(); ++stale; }
  out.printf("tx %u: %s (dropped %u stale)\n", (unsigned)n, hex, (unsigned)stale);
  const uint32_t t_write = millis();
  port.write(req, n);
  port.flush();
  const uint32_t t_sent = millis();
  uint8_t buf[64];
  size_t got = 0;
  uint32_t first_ms = 0;
  while (millis() - t_sent < window_ms) {
    if (port.available()) {
      const int c = port.read();
      if (c < 0) continue;
      if (got == 0) first_ms = millis() - t_sent;
      if (got < sizeof buf) buf[got] = static_cast<uint8_t>(c);
      ++got;
    } else {
      vTaskDelay(1);
    }
  }
  xSemaphoreGive(bus);
  const size_t kept = got < sizeof buf ? got : sizeof buf;
  powercell::pzem_hex(buf, kept, hex, sizeof hex);
  if (!got) {
    out.printf("rx 0 bytes in %lu ms (tx took %lu ms). No reply: PZEM did not answer or GPIO35 sees nothing.\n",
               (unsigned long)window_ms, (unsigned long)(t_sent - t_write));
    return false;
  }
  out.printf("rx %u bytes, first after %lu ms: %s\n", (unsigned)got, (unsigned long)first_ms, hex);
  const powercell::PzemReading rd = powercell::pzem_parse(cfg, buf, kept < 25 ? kept : 25, kept < 25);
  out.printf("parse status=%u valid=%d V=%.1f A=%.3f W=%.1f Wh=%lu\n", (unsigned)rd.status, (int)rd.valid,
             rd.voltage_dV / 10.0, rd.current_mA / 1000.0, rd.power_dW / 10.0, (unsigned long)rd.energy_Wh);
  return rd.valid;
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
