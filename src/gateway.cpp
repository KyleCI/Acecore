#include "power_guard.h"
#include "factory_id.h"
#include "gateway.h"
#include "log.h"
#include "ota.h"
#include "pins.h"
#include "power_policy.h"
#include "stm32_link.h"

namespace gateway {
namespace {
volatile bool g_on = false;
volatile int8_t g_req = -1;  // -1 none, 0 off, 1 on
const char *g_reqOrigin = "local";

void drive(bool on) {
  digitalWrite(PIN_PWR_KEY, on ? HIGH : LOW);
  digitalWrite(PIN_PWR_LIGHT, on ? HIGH : LOW);
}
}  // namespace

void earlyInit() {
  digitalWrite(PIN_PWR_KEY, LOW);
  pinMode(PIN_PWR_KEY, OUTPUT);
}

void begin() {
  earlyInit();
  digitalWrite(PIN_PWR_LIGHT, LOW);
  pinMode(PIN_PWR_LIGHT, OUTPUT);
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
    default: return "stm32_timeout";
  }
}

Result set(bool on, const char *origin) {
  using pc::PowerAction;
  // HQ payment_lock: refuse user/cloud on and off. System force-off (payment_lock,
  // battery_critical, ota) still runs. ON is never allowed while locked.
  if (fid::paymentLocked()) {
    bool systemOff = !on && origin && (!strcmp(origin, "payment_lock") || !strcmp(origin, "battery_critical") || !strcmp(origin, "ota"));
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
}

void request(bool on, const char *origin) { g_reqOrigin = origin; g_req = on ? 1 : 0; }

void service() {
  int8_t r = g_req;
  if (r < 0) return;
  g_req = -1;
  set(r == 1, g_reqOrigin);
}
}  // namespace gateway
