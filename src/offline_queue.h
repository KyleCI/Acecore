#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Offline event queue for actions taken while the backend was unreachable
// (local relay switches, socket pair/forget). Bounded RAM ring; checkpointed
// to NVS as one blob (routine events at most every OFFLINE_CHECKPOINT_MS,
// socket lifecycle events on the next loop pass). Flushed in batches of
// OFFLINE_BATCH_MAX with exponential backoff until offline_sync_ack.
// When full, the oldest routine event is dropped; socket_* events are never
// dropped (queue() returns false instead), matching the 1.0.4 semantics.
namespace offlineq {
void begin();
bool queue(const char *action, const char *socketId = "", const char *value = "");
void service(bool connected);         // loop(): checkpoint + flush
void flushNow();                      // send a batch immediately (on WS connect)
void acknowledge(JsonArrayConst ids); // from offline_sync_ack
size_t pending();
uint32_t dropped();
}  // namespace offlineq
