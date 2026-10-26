#pragma once
#include <ArduinoJson.h>
// Builders for every outbound JSON body. Field names of the 1.0.x firmware are
// kept; new fields are additive. Unmeasured values are null, never a fake 0.
namespace telemetry {
void buildTelemetry(JsonDocument &doc);   // "telemetry" body (without identity)
void buildSnapshot(JsonDocument &doc);    // "state_snapshot" body (without identity)
void buildMetrics(JsonDocument &doc);     // local GET /metrics (dashboard keys kept)
}  // namespace telemetry
