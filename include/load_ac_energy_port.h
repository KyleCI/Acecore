#pragma once

#include <ArduinoJson.h>

class AsyncWebServer;

// Original v0.1 adapter. It does not invent AC Vrms/Irms and it does not
// scale BMS discharge or any PZEM watt-hour.
void load_ac_energy_begin();
void load_ac_energy_poll();
void load_ac_energy_register(AsyncWebServer &server);
void load_ac_energy_fill(JsonObject dst, bool include_missing_path);
void load_ac_energy_fill_brief(JsonObject dst);
