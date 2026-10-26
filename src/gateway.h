#pragma once
#include <Arduino.h>
// Inverter on/off request ("gateway" in the app and backend). The ESP drives
// no relay: the STM32 owns the relay and the inverter. A request reaches it as
//   - framed link (STM32_LINK_PROTOCOL 1): set_mode INVERTER / OFF, confirmed by the STM32 ACK;
//   - legacy link: the PWR_KEY line. IO15 HIGH -> Q7 -> PB4 LOW = run the inverter.
// IO15 boots LOW (inverter not requested). PWR_LIGHT (IO25) follows the request.
namespace gateway {
void earlyInit();   // first call in setup(): IO15 LOW before anything else
void begin();
bool state();       // last state accepted by the STM32 (framed) or driven on PWR_KEY (legacy)
bool viaFramed();
enum class Result : uint8_t { Ok, Refused, Busy, Nack, Timeout, CriticalLatched, PaymentLocked };
const char *resultName(Result r);
// Blocking for the framed link (up to the set_mode timeout x retries): call from loop() context only.
Result set(bool on, const char *origin);
// Non-blocking, for async contexts (local web server): applied by service().
void request(bool on, const char *origin);
void service();
}  // namespace gateway
