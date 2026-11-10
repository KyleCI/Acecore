#include "power_guard.h"
#include "factory_id.h"
#include "gateway.h"
#include "hw_output.h"
#include "log.h"
#include "ota.h"
#include "pins.h"
#include "power_policy.h"
#include "stm32_link.h"
#if POWERCELL_DSP011
#include "driver/gpio.h"
#endif

static_assert(PIN_PWR_KEY == powercell::kDspEnableV011, "GPIO15 is the shared AC-enable pin");

namespace gateway {
namespace {
volatile bool g_on = false;
volatile int8_t g_req = -1;  // -1 none, 0 off, 1 on
const char *g_reqOrigin = "local";

#if POWERCELL_DSP011
// Drive the off level, then hold the pad. Hold survives a software reset, so
// the ROM and bootloader keep GPIO15 low after this process has run once.
// A cold power-on clears the hold; the constructor below is the first app code.
void latchDspEnableOff() {
  const gpio_num_t pin = static_cast<gpio_num_t>(PIN_PWR_KEY);
  const int level = powercell::v011_dsp_level(false);
  gpio_set_level(pin, level);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode(pin, GPIO_FLOATING);
  gpio_set_level(pin, level);
  gpio_hold_dis(pin);
  gpio_set_level(pin, level);
  gpio_hold_en(pin);
  g_on = false;
}
#endif

void drive(bool on) {
#if POWERCELL_DSP011
  const gpio_num_t pin = static_cast<gpio_num_t>(PIN_PWR_KEY);
  gpio_hold_dis(pin);
  gpio_set_level(pin, powercell::v011_dsp_level(on));
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  if (!on) gpio_hold_en(pin);
#else
  digitalWrite(PIN_PWR_KEY, on ? HIGH : LOW);
  digitalWrite(PIN_PWR_LIGHT, on ? HIGH : LOW);
#endif
}
}  // namespace

void earlyInit() {
#if POWERCELL_DSP011
  latchDspEnableOff();
#else
  digitalWrite(PIN_PWR_KEY, LOW);
  pinMode(PIN_PWR_KEY, OUTPUT);
#endif
}

void latchOutputOff() {
#if POWERCELL_DSP011
  latchDspEnableOff();
#endif
}

#if POWERCELL_DSP011
// After .bss and before setup(). Still after the ROM and the bootloader.
__attribute__((constructor(101))) static void dspEnableBootLatch() { latchDspEnableOff(); }
#endif

void begin() {
  earlyInit();
#if !POWERCELL_DSP011
  digitalWrite(PIN_PWR_LIGHT, LOW);
  pinMode(PIN_PWR_LIGHT, OUTPUT);
#endif
}

bool state() { return g_on; }
bool viaFramed() { return stm32link::protocol() == stm32link::Protocol::Framed; }

const char *resultName(Result r) {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::Refused: return "refused";
    case Result::Busy: return "stm32_busy";
    case Result::Nack: return "stm32_rejected";
    case Result::CriticalLatched: return "battery_critical_latched";
    case Result::PaymentLocked: return "payment_locked";
    case Result::OverloadLatched: return "output_overload_latched";
    default: return "stm32_timeout";
  }
}

Result set(bool on, const char *origin) {
  // HQ payment_lock: refuse user/cloud on and off. System force-off (payment_lock,
  // battery_critical, ota) still runs. ON is never allowed while locked.
  if (fid::paymentLocked()) {
    bool systemOff = !on && origin && (!strcmp(origin, "payment_lock") || !strcmp(origin, "battery_critical") ||
                                    !strcmp(origin, "ac_overload") || !strcmp(origin, "ota"));
    if (on || !systemOff) {
      LOGW("GATEWAY", "PAYMENT_LOCKED", "%s refused (origin=%s)", on ? "on" : "off", origin ? origin : "?");
      return Result::PaymentLocked;
    }
  }
  // Battery critical (ESP SoC latch or STM32 RSV latch): on is refused except the guard's own restore.
  if (on && powerguard::refuseOn(origin)) {
    LOGW("GATEWAY", "REFUSED", "on while battery critical latched (origin=%s)", origin);
    return Result::CriticalLatched;
  }
  if (on && powerguard::overloadLatched()) {
    LOGW("GATEWAY", "REFUSED", "on while ac overload latched (origin=%s)", origin);
    return Result::OverloadLatched;
  }
#if POWERCELL_DSP011
  if (on && ota::busy()) {
    LOGW("GATEWAY", "REFUSED", "on during firmware update (origin=%s)", origin);
    return Result::Refused;
  }
  g_on = on;
  drive(on);
  LOGI("GATEWAY", "STATE", "%s via dsp_gpio15 origin=%s", on ? "ON" : "OFF", origin ? origin : "?");
  return Result::Ok;
#else
  using pc::PowerAction;
  PowerAction act = pc::powerAction(on, ota::busy(), stm32link::online(), viaFramed());
  switch (act) {
    case PowerAction::RefuseOta:
      LOGW("GATEWAY", "REFUSED", "on during firmware update (origin=%s)", origin);
      return Result::Refused;
    case PowerAction::RefuseStm32Offline:
      LOGW("GATEWAY", "STM32_OFFLINE", "on refused: no STM32 status (origin=%s); PWR_KEY stays LOW", origin);
      return Result::Timeout;
    case PowerAction::SendSetMode: {
      uint8_t nack = 0;
      auto r = stm32link::setMode(on ? pc::frame::MODE_INVERTER : pc::frame::MODE_OFF, &nack);
      if (r != stm32link::Result::Ok) {
        LOGW("GATEWAY", "SET_MODE", "%s failed: %s nack=0x%02x origin=%s", on ? "ON" : "OFF", stm32link::resultName(r), nack, origin);
        if (r == stm32link::Result::Busy) return Result::Busy;
        if (r == stm32link::Result::Timeout) return Result::Timeout;
        return Result::Nack;  // includes ST_BAD_STATE while a fault is latched
      }
      g_on = on;
      digitalWrite(PIN_PWR_LIGHT, on ? HIGH : LOW);
      break;
    }
    case PowerAction::DrivePwrKey: g_on = true; drive(true); break;
    case PowerAction::ReleasePwrKey: g_on = false; drive(false); break;
  }
  LOGI("GATEWAY", "STATE", "%s via %s origin=%s", on ? "ON" : "OFF", act == PowerAction::SendSetMode ? "set_mode" : "pwr_key", origin);
  return Result::Ok;
#endif
}

void request(bool on, const char *origin) { g_reqOrigin = origin; g_req = on ? 1 : 0; }

void service() {
  int8_t r = g_req;
  if (r < 0) return;
  g_req = -1;
  set(r == 1, g_reqOrigin);
}
}  // namespace gateway
