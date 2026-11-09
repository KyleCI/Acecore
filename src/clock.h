#pragma once
#include <ArduinoJson.h>
#include "time_policy.h"
// Wall clock: NTP (authoritative), POST /v2/time from the phone, NVS last-good restore.
namespace clock_ {
void begin();    // restore tz and the last good time from NVS (source "nvs") when unsynced
void service();  // hourly save of the last good time
pc::clk::Verdict setFromPhone(int64_t epoch, bool hasTz, long tzOffsetMin);
int tzOffsetMin();
uint32_t ntpAtMs();  // millis() of the last NTP sync (0 = never)
void clearUserSettings();  // factory reset: forget the tz (keeps the last good time)
pc::clk::Source source();
void toJson(JsonObject o);
}  // namespace clock_
