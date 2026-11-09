#pragma once
#include <ArduinoJson.h>
// Builders for every outbound JSON body. Field names of the 1.0.x firmware are
// kept; new fields are additive. Unmeasured values are null, never a fake 0.
namespace telemetry {
void buildTelemetry(JsonDocument &doc);   // "telemetry" body (without identity)
void buildSnapshot(JsonDocument &doc);    // "state_snapshot" body (without identity)
void buildMetrics(JsonDocument &doc);
void appSolarToJson(JsonObject o);  // 2.5.8 GET /data root "solar" (power in kW for the app)     // local GET /metrics (dashboard keys kept)
// 2.5.4 app live view, flat keys (see README "App live fields"): source, status,
// output_voltage/current/power_w, load_pct, inverter_temperature, model,
// rated_kva, usable_kw, battery_current_a/power_w, remaining_ah,
// full_capacity_ah, cycle_count, soh, time_to_empty_min, time_to_full_min.
// Unknown values are null. Used in /data+SSE metrics, WS "live", /v2/status.
void liveToJson(JsonObject o);
void modelToJson(JsonObject o);           // model, rated_kva, usable_kw, rated_kw
}  // namespace telemetry
