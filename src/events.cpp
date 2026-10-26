#include "events.h"
#include "power_guard.h"
#include <time.h>
#include "backend_ws.h"
#include "battery.h"
#include "bms.h"
#include "config.h"
#include "events_core.h"
#include "log.h"
#include "offline_queue.h"
#include "sockets/sockets.h"
#include "stm32_link.h"

namespace events {
namespace {
using pc::ev::Event;
pc::ev::Engine g_engine;
SemaphoreHandle_t g_lock;
constexpr size_t kPending = 16, kRecent = 12;
Event g_pending[kPending]; size_t g_nPending = 0;
struct Recent { Event e; uint32_t seq; };
Recent g_recent[kRecent]; size_t g_recentHead = 0, g_nRecent = 0;
uint32_t g_seq = 0, g_last = 0;
void (*g_cb)(const char *, uint32_t) = nullptr;

uint32_t unixNow() { time_t t = time(nullptr); return t > 1700000000 ? (uint32_t)t : 0; }

void gatherSockets(pc::ev::Inputs &in) {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  if (!populateDeviceArray(a)) return;
  for (JsonObject o : a) {
    if (in.nSockets >= pc::ev::Inputs::kMaxSockets) break;
    pc::ev::SocketIn &s = in.sockets[in.nSockets++];
    strlcpy(s.id, o["deviceId"] | "", sizeof s.id);
    s.online = o["online"] | false;
    s.current = o["current"] | 0.0f;
    const char *al = o["alert"] | "";
    s.alertOvercurrent = !strcmp(al, "overcurrent");
    s.alertWarning = !strcmp(al, "current_high");
    s.lockout = o["lockout"] | false;
    s.lockoutA = o["lockout_trip_current"] | 0.0f;
    // relay_failed tracks relay switching only; a refused RESET_LOCKOUT keeps the lockout event active.
    if (strcmp(o["command_type"] | "", "reset_lockout")) strlcpy(s.cmd, o["command_status"] | "", sizeof s.cmd);
  }
}

const char *title(const Event &e) {
  using T = pc::ev::Type;
  switch (e.type) {
    case T::Overload: return e.active ? "Inverter overload" : "Inverter load back to normal";
    case T::Overcurrent: return e.socket[0] ? (e.active ? "Socket current too high" : "Socket current normal") : (e.active ? "Overcurrent fault" : "Overcurrent cleared");
    case T::InverterFault: return e.active ? "Inverter fault" : "Inverter fault cleared";
    case T::BmsFault: return e.active ? "Battery protection alarm" : "Battery alarm cleared";
    case T::RelayFailed: return e.active ? "Socket did not switch" : "Socket switching restored";
    case T::OverTemp: return e.active ? "Inverter too hot" : "Inverter temperature normal";
    case T::BatteryFull: return e.active ? "Battery full" : "Battery charging";
    case T::BatteryLow: return e.active ? "Battery low" : "Battery level recovered";
    case T::GridDown: return e.active ? "Grid power lost" : "Grid power restored";
    case T::ApplianceLockout: return e.active ? "Socket locked out after repeated trips" : "Socket lockout cleared";
    case T::BatteryCritical: return e.active ? "Battery critical: output off" : "Battery critical cleared";
    case T::OutputRestored: return "Output restored";
    case T::ModeFallback: return e.active ? "Smart mode paused: socket back on Manual" : "Smart mode resumed";
    case T::LoadShed: return e.active ? "Socket switched off to save power" : "Socket load shedding ended";
  }
  return "PowerCell alert";
}

bool dupOfBatteryAlert(const Event &e) { return !strcmp(e.id, "battery_soc"); }  // web_local already notifies SoC
void notif(JsonObject o, const Event &e, uint32_t seq) {
  { char idb[40]; snprintf(idb, sizeof idb, "event-%s-%lu", e.id, (unsigned long)seq); o["id"] = idb; } o["sequence"] = seq;
  o["title"] = title(e);
  { char mb[64]; snprintf(mb, sizeof mb, "%s%s%s%s", pc::ev::typeName(e.type), e.active ? " active" : " cleared", e.socket[0] ? " on socket " : "", e.socket); o["message"] = mb; }
  o["sound"] = e.active && e.severity != pc::ev::Sev::Info;
  eventToJson(o["event"].to<JsonObject>(), &e);
}

void record(const Event &e) {
  char line[96];
  snprintf(line, sizeof line, "%s %s sev=%s id=%s", pc::ev::typeName(e.type), e.active ? "on" : "off", pc::ev::sevName(e.severity), e.id);
  LOGI("EVENT", "TRANSITION", "%s", line);
  bool online = backend::connected();
  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (online && g_nPending < kPending) g_pending[g_nPending++] = e;
  else online = false;
  Recent &r = g_recent[g_recentHead]; r.e = e; r.seq = ++g_seq;
  g_recentHead = (g_recentHead + 1) % kRecent; if (g_nRecent < kRecent) ++g_nRecent;
  uint32_t seq = r.seq;
  xSemaphoreGive(g_lock);
  if (!online) {
    char action[28]; snprintf(action, sizeof action, "ev_%s", pc::ev::typeName(e.type));
    offlineq::queue(action, e.socket[0] ? e.socket : e.code, e.active ? "on" : "off");
  } else backend::requestTelemetry();
  if (g_cb && !dupOfBatteryAlert(e)) {
    JsonDocument d; notif(d.to<JsonObject>(), e, seq);
    static char s[512]; serializeJson(d, s, sizeof s); g_cb(s, seq);  // v12: no String churn
  }
}
}  // namespace

void begin() { g_lock = xSemaphoreCreateMutex(); }
void emit(const pc::ev::Event &e) { record(e); }
void onNotification(void (*cb)(const char *, uint32_t)) { g_cb = cb; }

void service() {
  uint32_t now = millis();
  if (now - g_last < 500) return;
  g_last = now;
  pc::ev::Inputs in;
  pc::Stm32Status s; uint32_t age = 0;
  in.stmOnline = stm32link::status(s, &age) && age < STM32_STALE_MS;
  if (in.stmOnline) {
    in.framed = s.has_ext; in.fault = s.fault; in.sys = s.sys; in.chr = s.chr;
    in.hasIout = s.has_iout; in.iout = s.iout; in.hasTemp = s.has_temp; in.temp = s.temp_c;
    in.hasGrid = s.has_grid; in.gridV = s.grid_v;
  }
  pc::jk::Data d;
  if ((in.bmsOnline = bms::read(d))) { in.bmsAlarms = d.alarms; in.bmsCurrentA = d.currentMa / 1000.0f; }
  battery::Reading b = battery::read();
  in.socValid = b.valid; in.soc = b.soc; in.socSource = b.source;
  in.critLatched = powerguard::espLatched();
  in.stmReserve = in.stmOnline && s.has_rsv && s.rsv_latched;
  gatherSockets(in);
  g_engine.update(in, now, unixNow());
  Event out[8]; size_t n;
  while ((n = g_engine.drain(out, 8)) > 0) for (size_t i = 0; i < n; ++i) record(out[i]);
  // Disconnected with events not yet in a telemetry message: hand them to the offline queue.
  if (!backend::connected() && g_nPending) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Event copy[kPending]; size_t k = g_nPending; memcpy(copy, g_pending, sizeof(Event) * k); g_nPending = 0;
    xSemaphoreGive(g_lock);
    for (size_t i = 0; i < k; ++i) {
      char action[28]; snprintf(action, sizeof action, "ev_%s", pc::ev::typeName(copy[i].type));
      offlineq::queue(action, copy[i].socket[0] ? copy[i].socket : copy[i].code, copy[i].active ? "on" : "off");
    }
  }
}

void eventToJson(JsonObject o, const void *p) {
  const Event &e = *static_cast<const Event *>(p);
  o["type"] = pc::ev::typeName(e.type);
  o["active"] = e.active;
  o["severity"] = pc::ev::sevName(e.severity);
  if (e.hasValue) o["value"] = e.value; else o["value"] = nullptr;
  if (e.socket[0]) o["socket"] = e.socket; else o["socket"] = nullptr;
  o["code"] = e.code;
  if (e.type == pc::ev::Type::ModeFallback || e.type == pc::ev::Type::LoadShed) o["reason"] = e.code;
  if (e.ts) o["ts"] = e.ts; else o["ts"] = nullptr;
  o["source"] = e.source;
  if (e.permanent) o["permanent"] = true;
  if (e.releaseRequires && e.active) o["release_requires"] = e.releaseRequires;
}

size_t appendPending(JsonArray arr) {
  if (!g_lock) return 0;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  size_t n = g_nPending;
  for (size_t i = 0; i < n; ++i) eventToJson(arr.add<JsonObject>(), &g_pending[i]);
  xSemaphoreGive(g_lock);
  return n;
}
void commitSent(size_t n) {
  if (!n) return;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (n > g_nPending) n = g_nPending;
  memmove(g_pending, g_pending + n, sizeof(Event) * (g_nPending - n)); g_nPending -= n;
  xSemaphoreGive(g_lock);
}
void activeToJson(JsonArray arr) {
  for (size_t i = 0; i < g_engine.activeCount(); ++i) if (const Event *e = g_engine.active(i)) eventToJson(arr.add<JsonObject>(), e);
}
void notificationsToJson(JsonArray arr) {
  if (!g_lock) return;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  for (size_t k = 0; k < g_nRecent; ++k) {
    const Recent &r = g_recent[(g_recentHead + kRecent - g_nRecent + k) % kRecent];
    if (!dupOfBatteryAlert(r.e)) notif(arr.add<JsonObject>(), r.e, r.seq);
  }
  xSemaphoreGive(g_lock);
}
}  // namespace events
