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
  const int pins[] = {PIN_LED_RED, PIN_LED_YELLOW, PIN_LED_GREEN, PIN_LED_AMBER};
  for (int p : pins) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
}
void service() {
  uint32_t now = millis();
  if (powerguard::lowPower()) {  // battery critical latched: dark except a 100 ms red blink every 4 s
    if (now - last < 100) return;
    last = now;
    digitalWrite(PIN_LED_RED, (now % 4000) < 100 ? HIGH : LOW);
    digitalWrite(PIN_LED_YELLOW, LOW); digitalWrite(PIN_LED_GREEN, LOW); digitalWrite(PIN_LED_AMBER, LOW);
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
  digitalWrite(PIN_LED_YELLOW, yellow && (!net::apActive() || blink) ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
  pc::jk::Data d;
  bool bmsOk = bms::read(d);
  bool amber = bmsOk ? (d.alarms != 0) : blink;
  digitalWrite(PIN_LED_AMBER, amber ? HIGH : LOW);
}
}  // namespace leds
