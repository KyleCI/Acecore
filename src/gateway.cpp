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
#include "esp_rom_gpio.h"
#endif

static_assert(PIN_PWR_KEY == powercell::kDspEnableV011, "GPIO15 is the shared AC-enable pin");
#if POWERCELL_DSP011
static_assert(PIN_FRONT_LIGHT == powercell::kDspFrontLightV011, "DSP front light is GPIO27");
#endif

namespace gateway {
namespace {
volatile bool g_on = false;
volatile int8_t g_req = -1;  // -1 none, 0 off, 1 on
const char *g_reqOrigin = "local";

#if POWERCELL_DSP011
// Drive the off level, then hold the pad. Hold survives a software reset, so
// the ROM and bootloader keep GPIO15 low after this process has run once.
// A cold power-on clears the hold; the constructor below is the first app code.
// GPIO15 resets into the MTDO (JTAG) IO_MUX function. Through cbf48bd/a1e3bad
// the DSP path only called gpio_set_level/gpio_set_direction, which never
// selected the GPIO function, so the pad ignored every write: ON did nothing.
void padWrite(bool on) {
  const gpio_num_t pin = static_cast<gpio_num_t>(PIN_PWR_KEY);
  powercell::PadStep steps[powercell::kDspPadSteps];
  const size_t n = powercell::v011_dsp_pad_steps(on, steps);
  for (size_t i = 0; i < n; ++i) {
    switch (steps[i].op) {
      case powercell::PadOp::HoldRelease: gpio_hold_dis(pin); break;
      case powercell::PadOp::SelectGpio: esp_rom_gpio_pad_select_gpio(PIN_PWR_KEY); break;
      case powercell::PadOp::PullsOff: gpio_set_pull_mode(pin, GPIO_FLOATING); break;
      case powercell::PadOp::SetLevel: gpio_set_level(pin, steps[i].level); break;
      case powercell::PadOp::OutputEnable: gpio_set_direction(pin, GPIO_MODE_OUTPUT); break;
      case powercell::PadOp::HoldEnable: gpio_hold_en(pin); break;
    }
  }
}

// Front light follows the inverter: same call, same order, every on and off path.
void frontLight(bool on) {
  const int level = powercell::v011_front_light_level(on) ? HIGH : LOW;
  digitalWrite(PIN_FRONT_LIGHT, level);
  pinMode(PIN_FRONT_LIGHT, OUTPUT);
  digitalWrite(PIN_FRONT_LIGHT, level);
}

void latchDspEnableOff() {
  padWrite(false);
  frontLight(false);
  g_on = false;
}
#endif

void drive(bool on) {
#if POWERCELL_DSP011
  padWrite(on);
  frontLight(on);
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
    case Result::OverTempLatched: return "over_temperature_latched";
    default: return "stm32_timeout";
  }
}

Result set(bool on, const char *origin) {
  // HQ payment_lock: refuse user/cloud on and off. System force-off (payment_lock,
  // battery_critical, ota) still runs. ON is never allowed while locked.
  if (fid::paymentLocked()) {
    bool systemOff = !on && origin && (!strcmp(origin, "payment_lock") || !strcmp(origin, "battery_critical") ||
                                    !strcmp(origin, "ac_overload") || !strcmp(origin, "ota") ||
                                    !strcmp(origin, "over_temperature"));
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
  // DSP system NTC: >= 80 C, or latched and not yet valid and < 70 C. Checked last so a
  // refused ON for another reason never clears the over-temperature latch.
  if (on && powerguard::overTempRefuseOn(origin)) {
    LOGW("GATEWAY", "REFUSED", "on while over temperature (origin=%s)", origin);
    return Result::OverTempLatched;
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
