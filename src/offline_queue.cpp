#include "prefs_safe.h"
#include "offline_queue.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include "backend_ws.h"
#include "config.h"
#include "log.h"
#include "ring_queue.h"
#include "settings.h"

namespace offlineq {
namespace {
struct Event {
  char id[32];
  char action[28];
  char socket[13];
  char value[8];
  uint32_t unixTime;  // 0 if the clock was not set when queued
  uint32_t bootMs;
  uint16_t bootCount;
};
pc::RingQueue<Event, OFFLINE_QUEUE_CAPACITY> q;
SemaphoreHandle_t mx = nullptr;
bool dirty = false, urgent = false;
uint32_t lastCheckpoint = 0, nextAttempt = 0, backoff = 2000, counter = 0;
uint16_t bootCount = 0;
uint32_t dropCount = 0;

bool isLifecycle(const Event &e) { return !strncmp(e.action, "socket_", 7); }

void checkpoint() {
  static Event snap[OFFLINE_QUEUE_CAPACITY];
  size_t n;
  xSemaphoreTake(mx, portMAX_DELAY);
  n = q.size();
  for (size_t i = 0; i < n; ++i) snap[i] = *q.peek(i);
  dirty = urgent = false;
  xSemaphoreGive(mx);
  Preferences p;
  if (!p.begin("pc_offline", false)) return;
  if (n) p.putBytes("events", snap, n * sizeof(Event));
  else p.remove("events");
  p.end();
  lastCheckpoint = millis();
}
}  // namespace

void begin() {
  mx = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin("pc_offline", false)) {
    bootCount = p.getUShort("boots", 0) + 1;
    p.putUShort("boots", bootCount);
    size_t len = prefsLen(p, "events");
    if (len && len % sizeof(Event) == 0 && len <= sizeof(Event) * OFFLINE_QUEUE_CAPACITY) {
      static Event tmp[OFFLINE_QUEUE_CAPACITY];
      p.getBytes("events", tmp, len);
      for (size_t i = 0; i < len / sizeof(Event); ++i) q.push(tmp[i]);
    }
    p.end();
  }
  LOGI("SYNC", "RESTORE", "%u offline events pending", (unsigned)q.size());
}

bool queue(const char *action, const char *socketId, const char *value) {
  Event e{};
  time_t now = time(nullptr);
  e.unixTime = now >= 1700000000 ? static_cast<uint32_t>(now) : 0;
  e.bootMs = millis();
  e.bootCount = bootCount;
  strlcpy(e.action, action, sizeof e.action);
  strlcpy(e.socket, socketId ? socketId : "", sizeof e.socket);
  strlcpy(e.value, value ? value : "", sizeof e.value);
  xSemaphoreTake(mx, portMAX_DELAY);
  snprintf(e.id, sizeof e.id, "%s-%x-%lx", settings::id().deviceId, bootCount, (unsigned long)++counter);
  if (q.size() == q.capacity()) {
    // drop the oldest routine event; refuse if everything left is lifecycle
    bool removedOne = false;
    q.removeIf([&](const Event &x) { if (removedOne || isLifecycle(x)) return false; removedOne = true; return true; });
    if (removedOne) ++dropCount;
    if (!removedOne) { xSemaphoreGive(mx); LOGE("SYNC", "FULL", "refusing %s: queue full of lifecycle events", action); return false; }
  }
  q.push(e);
  dirty = true;
  if (isLifecycle(e)) urgent = true;
  size_t n = q.size();
  xSemaphoreGive(mx);
  LOGI("SYNC", "QUEUED", "%s socket=%s value=%s pending=%u", action, e.socket, e.value, (unsigned)n);
  return true;
}

void flushNow() { nextAttempt = 0; backoff = 2000; }

void service(bool connected) {
  uint32_t now = millis();
  if (dirty && (urgent || now - lastCheckpoint >= OFFLINE_CHECKPOINT_MS)) checkpoint();
  if (!connected || q.empty() || (int32_t)(now - nextAttempt) < 0) return;
  JsonDocument doc;
  doc["type"] = "offline_sync";
  backend::addIdentity(doc);
  doc["gateway_name"] = settings::id().gatewayName;
  JsonArray events = doc["events"].to<JsonArray>();
  xSemaphoreTake(mx, portMAX_DELAY);
  for (size_t i = 0; i < q.size() && i < OFFLINE_BATCH_MAX; ++i) {
    const Event *e = q.peek(i);
    JsonObject o = events.add<JsonObject>();
    o["event_id"] = e->id;
    o["action"] = e->action;
    if (e->socket[0]) o["socket_id"] = e->socket;
    if (e->value[0]) o["value"] = e->value;
    if (e->unixTime) o["created_at"] = e->unixTime;
    o["created_ms"] = e->bootMs;       // legacy field: ms since boot
    o["boot_count"] = e->bootCount;    // lets the server order events across reboots
  }
  xSemaphoreGive(mx);
  backend::send(doc);
  nextAttempt = now + backoff;               // resend only if no ack arrives
  backoff = backoff < 60000 ? backoff * 2 : 60000;
}

void acknowledge(JsonArrayConst ids) {
  xSemaphoreTake(mx, portMAX_DELAY);
  size_t removed = q.removeIf([&](const Event &e) {
    for (JsonVariantConst id : ids) if (!strcmp(id.as<const char *>() ? id.as<const char *>() : "", e.id)) return true;
    return false;
  });
  if (removed) { dirty = true; urgent = true; }
  size_t left = q.size();
  xSemaphoreGive(mx);
  nextAttempt = 0; backoff = 2000;  // next batch right away
  LOGI("SYNC", "ACK", "removed=%u pending=%u", (unsigned)removed, (unsigned)left);
}

size_t pending() { return q.size(); }
uint32_t dropped() { return dropCount; }
}  // namespace offlineq
