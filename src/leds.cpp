#include "power_guard.h"
#include "leds.h"
#include <Arduino.h>
#include "backend_ws.h"
#include "battery.h"
#include "bms.h"
#include "net.h"
#include "pins.h"
#include "stm32_link.h"
#if POWERCELL_DSP011
#include "dsp_status_led.h"
#include "grid_sense.h"
#endif

namespace leds {
namespace {
uint32_t last = 0;
#if POWERCELL_DSP011
// LEDC channels for red, blue, green (StatusColor order). Nothing else in this
// firmware uses LEDC.
constexpr uint8_t kCh[3] = {0, 1, 2};
#endif
}
void begin() {
#if POWERCELL_DSP011
  // Orange (PIN_LED_BLUE) is GPIO12 (MTDI). Drive it low before it becomes an
  // output and never enable a pull-up. Red is 13, green is 14 (Caleb 2026-11-12
  // swapped orange and red). Then hand each pin to its own LEDC channel
  // (5 kHz, 8-bit) at duty 0, as Dubem's initLights().
  // The front light (GPIO27) is owned by gateway: it follows the inverter.
  const int pins[] = {PIN_LED_RED, PIN_LED_BLUE, PIN_LED_GREEN};
  for (int p : pins) { digitalWrite(p, LOW); pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  for (int i = 0; i < 3; ++i) {
    ledcSetup(kCh[i], powercell::kLedPwmFreqHz, powercell::kLedPwmBits);
    ledcAttachPin(pins[i], kCh[i]);
    ledcWrite(kCh[i], 0);
  }
#else
  const int pins[] = {PIN_LED_RED, PIN_LED_YELLOW, PIN_LED_GREEN, PIN_LED_AMBER};
  for (int p : pins) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
#endif
}
void service() {
  uint32_t now = millis();
#if POWERCELL_DSP011
  // Battery status light exactly as Dubem's lightUpStatus(). Replaces, on DSP
  // only, the original profile's indications (critical-latch red blink,
  // STM32 fault red, offline/hotspot/low-SOC blue). 20 ms steps keep the
  // breathe smooth.
  if (now - last < 20) return;
  last = now;
  pc::jk::Data d;
  uint32_t age = 0;
  const bool fresh = bms::sample(d, &age, powercell::kLedBmsStaleMs);
  DspGridReading grid;
  const bool gridPresent = dspGridRead(&grid) && grid.valid && grid.present;
  const powercell::StatusLed out = powercell::dsp_status_led(fresh, fresh ? d.soc : 0, gridPresent, now);
  for (int i = 0; i < 3; ++i) ledcWrite(kCh[i], i == static_cast<int>(out.color) ? out.duty : 0);
#else
  if (powerguard::lowPower()) {  // battery critical latched: dark except a 100 ms red blink every 4 s
    if (now - last < 100) return;
    last = now;
    digitalWrite(PIN_LED_RED, (now % 4000) < 100 ? HIGH : LOW);
#if POWERCELL_DSP011
    digitalWrite(PIN_LED_BLUE, LOW); digitalWrite(PIN_LED_GREEN, LOW);
#else
    digitalWrite(PIN_LED_YELLOW, LOW); digitalWrite(PIN_LED_GREEN, LOW); digitalWrite(PIN_LED_AMBER, LOW);
#endif
    return;
  }
  if (now - last < 500) return;
  last = now;
  battery::Reading b = battery::read();
  pc::Stm32Status s;
  bool fault = stm32link::status(s) && stm32link::online() && (s.sys == pc::SysState::Fault || (s.fault & 0xFF));
  bool red = fault || (b.valid && b.soc < 10);
  bool yellow = !red && (!backend::connected() || net::apActive() || (b.valid && b.soc < 25));
  bool green = !red && !yellow;
  bool blink = (now / 500) & 1;
  digitalWrite(PIN_LED_RED, red ? HIGH : LOW);
#if POWERCELL_DSP011
  // This board has blue where the original profile has yellow. No amber LED;
  // GPIO32 is PZEM TX and GPIO35 is PZEM RX.
  digitalWrite(PIN_LED_BLUE, yellow && (!net::apActive() || blink) ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
#else
  digitalWrite(PIN_LED_YELLOW, yellow && (!net::apActive() || blink) ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
  pc::jk::Data d;
  bool bmsOk = bms::read(d);
  bool amber = bmsOk ? (d.alarms != 0) : blink;
  digitalWrite(PIN_LED_AMBER, amber ? HIGH : LOW);
#endif
#endif  // POWERCELL_DSP011
}
}  // namespace leds
