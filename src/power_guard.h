#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
// Battery SoC rules (fw 2.2), logic in lib/pc_core/src/soc_guard.h:
//  socket shedding (Heavy 40/50 %, Essential 20/30 %), critical shutdown at 10 %
//  latched in NVS, restore at >= 25 % with > +1 A charge held 60 s, low-power standby
//  while latched. Also honours the STM32 reserve latch (legacy RSV:1).
namespace powerguard {
void begin();
void service();
bool espLatched();           // ESP SoC <= 10 % latch (NVS)
bool stmReserve();           // STM32 RSV:1 latch reported
bool latched();              // battery critical: on commands are refused (battery_critical_latched)
bool overloadLatched();      // AC-output overload shutdown (output_overload_latched). DSP only.
bool lowPower();             // standby mode active (== latched())
bool refuseOn(const char *origin);
// DSP over-temperature cutoff (system NTC >= 80 C, latched in NVS). STM: always false.
bool overTempLatched();
bool overTempBlocksOn();               // ON would be refused now (no state change)
bool overTempRefuseOn(const char *origin);  // ON gate: clears the latch when valid and < 70 C  // battery gate only. "soc_restore" passes while the ESP latch is set.
void setPriority(const String &socketId, bool essential);  // console PRIO fallback: writes the socket config priority
void setAlertThresholds(float lowPercent, float warningPercent);
void toJson(JsonObject o);
String debugJson();
}  // namespace powerguard
