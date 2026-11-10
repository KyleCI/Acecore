#pragma once
#include <ArduinoJson.h>
// 2.2 event engine glue (rules in pc_core/events_core). service() samples the
// STM32, BMS, battery and sockets every 500 ms. Each transition goes to:
//   - telemetry events[] (the next WS telemetry, sent immediately), or the
//     offline queue as ev_<type> when the backend is not connected
//   - the local GET /data notifications[] and the /events SSE "notification"
namespace pc { namespace ev { struct Event; } }
namespace events {
void emit(const pc::ev::Event &e);  // socket config engine transitions (mode_fallback, load_shed)
void begin();
void service();
// Telemetry: appends unsent transitions; commit after a successful send.
size_t appendPending(JsonArray arr);
void commitSent(size_t n);
void activeToJson(JsonArray arr);      // currently active conditions
// Local API: recent transitions as app notifications (id, sequence, title, message, sound).
void notificationsToJson(JsonArray arr);
void eventToJson(JsonObject o, const void *ev);  // ev is a pc::ev::Event
// Called for each new local notification (SSE); json is one notification object.
void onNotification(void (*cb)(const char *json, uint32_t seq));
}  // namespace events
