#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

// Factory image rating and the Manual ceiling / battery saves.
// Stock esp32dev is unrated. The hardware images carry 5000, 3500 or 1500 (1500 provisional).
void manualLimitsBegin();
void manualLimitsService(uint32_t now_ms);
void manualSetupRating(JsonDocument& doc);
void manualStatusRating(JsonObject obj);
// auth: 0 missing, 1 session, 2 lan token. Returns the HTTP status it wrote.
int manualSystemRating(JsonDocument& out, int auth, bool post);
int manualAcSettings(JsonDocument& doc, JsonDocument& out, int auth, bool post);
int manualBatterySettings(JsonDocument& doc, JsonDocument& out, int auth, bool post);
