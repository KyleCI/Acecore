#include "power_guard.h"
#include "leds.h"
#include <Arduino.h>
#include "backend_ws.h"
#include "battery.h"
#include "bms.h"
#include "net.h"
#include "pins.h"
#include "stm32_link.h"

namespace leds {
namespace { uint32_t last = 0; }
void begin() {
#if POWERCELL_DSP011
  // Red is GPIO12 (MTDI). Drive it low before it becomes an output and never
  // enable a pull-up. Blue is 13, green is 14. Front light starts off.
  const int pins[] = {PIN_LED_RED, PIN_LED_BLUE, PIN_LED_GREEN, PIN_FRONT_LIGHT};
  for (int p : pins) { digitalWrite(p, LOW); pinMode(p, OUTPUT); digitalWrite(p, LOW); }
#else
  const int pins[] = {PIN_LED_RED, PIN_LED_YELLOW, PIN_LED_GREEN, PIN_LED_AMBER};
  for (int p : pins) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
#endif
}
void service() {
  uint32_t now = millis();
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
  // GPIO32 is the PZEM receiver.
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
}
}  // namespace leds
