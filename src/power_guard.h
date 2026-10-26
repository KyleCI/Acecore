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
bool latched();              // either: on commands are refused (battery_critical_latched)
bool lowPower();             // standby mode active (== latched())
bool refuseOn(const char *origin);  // gateway::set hook; only "soc_restore" passes while latched
void setPriority(const String &socketId, bool essential);  // console PRIO fallback: writes the socket config priority
void toJson(JsonObject o);
String debugJson();
}  // namespace powerguard
