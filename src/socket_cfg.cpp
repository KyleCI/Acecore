#include "prefs_safe.h"
#include "socket_cfg.h"
#include <Preferences.h>
#include <map>
#include <time.h>
#include "backend_ws.h"
#include "clock.h"
#include "events.h"
#include "events_core.h"
#include "log.h"
#include "sockets/sockets.h"
#include "socket_rules.h"
#include "factory_id.h"

namespace socketcfg {
namespace {
using namespace pc::sock;
const char *NS = "sock_cfg", *NS_PRIO = "sock_prio";  // sock_prio: interim PRIO values, read once as the default
constexpr uint8_t kLimitTries = 5;
constexpr uint32_t kLimitRetryMs = 60000;
struct Sock {
  Config cfg; bool stored = false;
  State st;
  Decision last;
  bool byUs = false, restoreOn = false;    // we switched it off; it was on before
  uint8_t overN = 0; bool safety = false; unsigned long lastRx = 0;
  uint8_t limitTries = 0; uint32_t limitAt = 0;
  Fallback fbSent = Fallback::None; Reason shedSent = Reason::None;
  float limitConfirmed = -1;
};
std::map<String, Sock> g;
SemaphoreHandle_t g_mx;
uint32_t g_cloudDownAt = 0;
struct Lock { Lock() { xSemaphoreTake(g_mx, portMAX_DELAY); } ~Lock() { xSemaphoreGive(g_mx); } };

String key(String id) { id.replace(":", ""); id.toUpperCase(); return id.substring(0, 15); }
Sock &get(const String &id) {
  auto it = g.find(id);
  if (it != g.end()) return it->second;
  Sock &s = g[id];
  Preferences p;
  if (prefsOpenRO(p, NS)) {
    if (prefsLen(p, id.c_str()) == sizeof(Config)) { p.getBytes(id.c_str(), &s.cfg, sizeof(Config)); s.stored = s.cfg.ver == 1; }
    p.end();
  }
  if (!s.stored) {
    s.cfg = Config();
    if (prefsOpenRO(p, NS_PRIO)) { if (p.getUChar(id.c_str(), 0)) s.cfg.prio = Prio::Essential; p.end(); }
  }
  return s;
}
bool save(const String &id, Sock &s) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = p.putBytes(id.c_str(), &s.cfg, sizeof(Config)) == sizeof(Config);
  p.end();
  if (ok) s.stored = true;
  return ok;
}
bool clockSynced() { pc::clk::Source c = clock_::source(); return (c == pc::clk::Source::Ntp || c == pc::clk::Source::Phone) && time(nullptr) > 1700000000; }
uint32_t unixNow() { time_t t = time(nullptr); return t > 1700000000 ? (uint32_t)t : 0; }

void emit(pc::ev::Type t, bool active, const char *code, const String &id, bool hasV = false, float v = 0) {
  pc::ev::Event e;
  e.type = t; e.active = active; e.severity = active && t == pc::ev::Type::LoadShed ? pc::ev::Sev::Warning : pc::ev::Sev::Info;
  snprintf(e.id, sizeof e.id, "%s_%s", t == pc::ev::Type::LoadShed ? "shed" : "fallback", id.c_str());
  strlcpy(e.socket, id.c_str(), sizeof e.socket); strlcpy(e.code, code ? code : "", sizeof e.code);
  e.hasValue = hasV; e.value = v; e.ts = unixNow(); e.source = "esp";
  events::emit(e);
}
bool isShedReason(Reason r) { return r == Reason::SafetyTrip || r == Reason::SocHeavy || r == Reason::SocEssential || r == Reason::InverterOverload; }

// ESP backup monitor: the socket trips itself at its user limit (20 A hard). If a fresh reading
// stays above the limit + 0.5 A with the relay on for 4 readings, the ESP sends RELAY_OFF.
void monitor(Sock &s, JsonObject o) {
  bool on = (o["relayState"] | 0) == 1;
  if (!on) { s.overN = 0; s.safety = false; return; }
  unsigned long age = o["last_seen_age_ms"] | 0UL, rx = millis() - age;
  if (rx == s.lastRx) return;
  s.lastRx = rx;
  float lim = s.cfg.limitDa / 10.0f, a = o["current"] | 0.0f;
  s.overN = a >= lim + 0.5f || a >= kTripAmps ? s.overN + 1 : 0;
  if (s.overN >= 4) s.safety = true;
}
}  // namespace

void begin() { g_mx = xSemaphoreCreateMutex(); }

void pass(bool socValid, float soc, bool gridDown, bool latched, bool overload) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  populateDeviceArray(arr);
  uint32_t now = millis();
  bool up = backend::connected();
  if (up) g_cloudDownAt = 0; else if (!g_cloudDownAt) g_cloudDownAt = now ? now : 1;
  In in;
  in.socValid = socValid; in.soc = soc; in.gridDown = gridDown; in.latched = latched; in.overload = overload;
  in.cloudUp = up || now - g_cloudDownAt < kCloudGraceMs;
  in.clockSynced = clockSynced(); in.nowMs = now;
  if (in.clockSynced) {
    time_t t = time(nullptr) + clock_::tzOffsetMin() * 60; struct tm tm; gmtime_r(&t, &tm);
    in.minutes = (uint16_t)(tm.tm_hour * 60 + tm.tm_min); in.wday = (uint8_t)tm.tm_wday;
  }
  Lock l;
  for (JsonObject o : arr) {
    String id = o["deviceId"] | "";
    if (!id.length()) continue;
    Sock &s = get(id);
    float lc = o["limit_confirmed_amps"] | -1.0f; s.limitConfirmed = lc;
    if (!(o["online"] | false)) continue;
    monitor(s, o);
    in.safety = s.safety;
    Decision d = decide(s.cfg, s.st, in);
    // mode_fallback events
    if (s.st.fb != s.fbSent) {
      if (s.fbSent != Fallback::None) emit(pc::ev::Type::ModeFallback, false, fallbackName(s.fbSent), id);
      if (s.st.fb != Fallback::None) emit(pc::ev::Type::ModeFallback, true, fallbackName(s.st.fb), id);
      LOGW("SOCK", "MODE_FALLBACK", "socket=%s %s", id.c_str(), s.st.fb != Fallback::None ? fallbackName(s.st.fb) : "cleared");
      s.fbSent = s.st.fb;
    }
    bool relayOn = (o["relayState"] | 0) == 1;
    bool busy = !strcmp(o["command_status"] | "", "pending");
    // SET_LIMIT until the socket reports the same LIM (old sockets never do: 5 tries, then pending stays).
    uint8_t rep = lc > 0 ? (uint8_t)lroundf(lc * 10) : 0;
    if (rep != s.cfg.limitDa && !busy && s.limitTries < kLimitTries && (!s.limitAt || now - s.limitAt >= kLimitRetryMs)) {
      if (sendSetLimit(id, s.cfg.limitDa)) { s.limitAt = now; ++s.limitTries; busy = true; LOGI("SOCK", "SET_LIMIT", "socket=%s %.1fA try=%u", id.c_str(), s.cfg.limitDa / 10.0f, s.limitTries); }
    }
    if (busy) continue;
    // load_shed events follow the automatic off reasons
    Reason sr = isShedReason(d.reason) ? d.reason : Reason::None;
    if (sr != s.shedSent) {
      if (s.shedSent != Reason::None) emit(pc::ev::Type::LoadShed, false, reasonName(s.shedSent), id);
      if (sr != Reason::None) emit(pc::ev::Type::LoadShed, true, reasonName(sr), id, sr != Reason::InverterOverload, sr == Reason::SafetyTrip ? (o["current"] | 0.0f) : soc);
      s.shedSent = sr;
    }
    // Commands only on decision changes (a user may switch the socket by hand in between),
    // continuously off while the battery is latched.
    bool changed = d.want != s.last.want || d.reason != s.last.reason;
    if (d.want == 0 && relayOn && (changed || latched)) {
      if (!sendRelayCommand(id, false)) continue;  // retry next pass
      if (!s.byUs) s.restoreOn = true;
      s.byUs = true;
      LOGW("SOCK", "OFF", "socket=%s reason=%s soc=%.0f", id.c_str(), reasonName(d.reason), soc);
    } else if (d.want == 1 && changed) {
      if (fid::paymentLocked()) { s.last = d; continue; }  // HQ lock: no auto ON
      if (!relayOn && !sendRelayCommand(id, true)) continue;
      s.byUs = false;
      LOGI("SOCK", "ON", "socket=%s reason=%s", id.c_str(), reasonName(d.reason));
    } else if (d.want < 0 && changed) {
      // A schedule window ended: the inverse action. An automatic off ended: restore (not after safety).
      bool on = s.last.reason == Reason::Schedule ? s.last.want == 0 : s.byUs && s.restoreOn && s.last.reason != Reason::SafetyTrip;
      if (on && fid::paymentLocked()) on = false;
      if (s.last.reason == Reason::Schedule || on) {
        if (relayOn != on && !sendRelayCommand(id, on)) continue;
        LOGI("SOCK", "RESUME", "socket=%s %s after %s", id.c_str(), on ? "on" : "off", reasonName(s.last.reason));
      }
      s.byUs = false; s.restoreOn = false;
    }
    if (d.want == 0 && !relayOn && changed && !s.byUs) s.restoreOn = false;  // already off: nothing to restore
    s.last = d;
  }
}

const char *apply(JsonObjectConst c, const char *typeHint, const String &sockHint, String &out) {
  String type = c["type"] | (typeHint ? typeHint : "");
  type.toUpperCase();
  String id = c["socket"] | sockHint.c_str();
  if (!id.length()) id = c["socket_id"] | "";
  id = key(id);
  out = id;
  if (id.length() != 12 || !checkRegistered(id)) return "unknown_socket";
  Lock l;
  Sock &s = get(id);
  if (type == "SET_OVERRIDE") {
    const char *src = c["source"] | "", *act = c["action"] | "";
    bool w = !strcmp(src, "weather");
    if (!w && strcmp(src, "ai")) return "invalid_source";
    if (strcmp(act, "on") && strcmp(act, "off")) return "invalid_action";
    if (w && s.cfg.prio == Prio::Essential) return "weather_heavy_only";
    int64_t exp = c["expires_at"] | (int64_t)0, ttl = c["ttl_seconds"] | (int64_t)kTtlS;
    uint32_t ms = overrideMs(exp, unixNow(), clockSynced(), ttl);
    if (!ms) return "override_expired";
    s.st.ov.set = true; s.st.ov.on = act[1] == 'n'; s.st.ov.weather = w; s.st.ov.untilMs = millis() + ms; s.st.ovExpired = false;
    LOGI("SOCK", "OVERRIDE", "socket=%s %s %s for %lus", id.c_str(), src, act, (unsigned long)(ms / 1000));
    return nullptr;
  }
  Config n = s.cfg;
  JsonObjectConst cfg = type == "SET_CONFIG" ? c["config"].as<JsonObjectConst>() : c;
  if (type == "SET_CONFIG" && cfg.isNull()) return "invalid_config";
  if (type != "SET_CONFIG" && type != "SET_LIMIT") return "invalid_type";
  JsonVariantConst lim = cfg["limit_amps"].isNull() ? cfg["overload_limit_a"] : cfg["limit_amps"];
  if (!lim.isNull()) {
    if (!lim.is<float>() || !limitDa(lim.as<float>(), n.limitDa, nullptr)) return "invalid_limit";
  } else if (type == "SET_LIMIT") return "invalid_limit";
  if (type == "SET_CONFIG") {
    if (!cfg["trip_amps"].isNull() && (cfg["trip_amps"] | 0) != kTripAmps) return "trip_amps_fixed";
    if (cfg["name"].is<const char *>()) {
      const char *nm = cfg["name"]; if (strlen(nm) > kNameMax) return "invalid_name";
      strlcpy(n.name, nm, sizeof n.name);
    }
    if (!cfg["mode"].isNull() && !parseMode(cfg["mode"] | "", n.mode)) return "invalid_mode";
    if (!cfg["last_user_mode"].isNull()) { if (!parseMode(cfg["last_user_mode"] | "", n.lastUserMode)) return "invalid_mode"; }
    else if (!cfg["mode"].isNull()) n.lastUserMode = n.mode;
    if (!cfg["priority"].isNull() && !parsePrio(cfg["priority"] | "", n.prio)) return "invalid_priority";
    if (!cfg["day_start"].isNull() && !parseHHMM(cfg["day_start"] | "", n.dayStart)) return "invalid_time";
    if (!cfg["night_start"].isNull() && !parseHHMM(cfg["night_start"] | "", n.nightStart)) return "invalid_time";
    if (!cfg["schedules"].isNull()) {
      JsonArrayConst a = cfg["schedules"].as<JsonArrayConst>();
      if (a.isNull() || a.size() > kMaxSched) return "invalid_schedules";
      n.nSched = 0;
      for (JsonObjectConst w : a) {
        Sched &x = n.sched[n.nSched];
        int m = w["daymask"] | 0; const char *act = w["action"] | "";
        if (m < 1 || m > 127 || !parseHHMM(w["start"] | "", x.start) || !parseHHMM(w["end"] | "", x.end) || (strcmp(act, "on") && strcmp(act, "off")))
          return "invalid_schedules";
        x.mask = (uint8_t)m; x.action = act[1] == 'n'; ++n.nSched;
      }
    }
    JsonObjectConst sh = cfg["shed"];
    if (!sh.isNull()) {
      int ho = sh["heavy_off_soc"] | (int)n.heavyOff, hn = sh["heavy_on_soc"] | (int)n.heavyOn;
      int eo = sh["essential_off_soc"] | (int)n.essOff, en = sh["essential_on_soc"] | (int)n.essOn;
      if (!validShed(ho, hn) || !validShed(eo, en)) return "invalid_shed";
      n.heavyOff = ho; n.heavyOn = hn; n.essOff = eo; n.essOn = en;
    }
    if (n.lastUserMode != s.cfg.lastUserMode) s.st.ovExpired = false;
  }
  if (n.limitDa != s.cfg.limitDa) { s.limitTries = 0; s.limitAt = 0; }
  s.cfg = n;
  if (!save(id, s)) return "storage_failed";
  LOGI("SOCK", type.c_str(), "socket=%s limit=%.1fA mode=%s prio=%s sched=%u", id.c_str(), n.limitDa / 10.0f, modeName(n.lastUserMode), prioName(n.prio), n.nSched);
  return nullptr;
}

namespace {
void stateJsonL(const String &id, JsonObject o) {
  Sock &s = get(id);
  const Config &c = s.cfg;
  char t[6];
  o["name"] = c.name;
  o["limit_amps"] = o["overload_limit_a"] = c.limitDa / 10.0f;
  if (s.limitConfirmed > 0) o["limit_confirmed_amps"] = s.limitConfirmed; else o["limit_confirmed_amps"] = nullptr;
  o["limit_pending"] = s.limitConfirmed <= 0 || lroundf(s.limitConfirmed * 10) != c.limitDa;
  o["mode"] = modeName(effectiveMode(c, s.st));
  o["last_user_mode"] = modeName(c.lastUserMode);
  if (const char *f = fallbackName(s.st.fb)) o["mode_fallback"] = f; else o["mode_fallback"] = nullptr;
  o["priority"] = prioName(c.prio);
  fmtHHMM(c.dayStart, t); o["day_start"] = t;
  fmtHHMM(c.nightStart, t); o["night_start"] = t;
  o["schedule_count"] = c.nSched;
  if (s.st.ov.set) {
    JsonObject v = o["override"].to<JsonObject>();
    v["action"] = s.st.ov.on ? "on" : "off"; v["source"] = s.st.ov.weather ? "weather" : "ai";
    v["expires_in"] = (s.st.ov.untilMs - millis()) / 1000;
  } else o["override"] = nullptr;
  o["clock_synced"] = clockSynced();
  o["trip_amps"] = kTripAmps;
  o["config_stored"] = s.stored;
}
}  // namespace
void stateJson(const String &id, JsonObject o) { Lock l; stateJsonL(id, o); }

void decorate(JsonArray a) {
  Lock l;
  for (JsonObject o : a) {
    String id = o["deviceId"] | "";
    if (!id.length() || (o["available_to_pair"] | false)) continue;
    Sock &s = get(id);
    stateJsonL(id, o);
    bool on = (o["relayState"] | 0) == 1;
    const char *r = nullptr;
    if (o["lockout"] | false) r = "appliance_lockout";
    else if (!on) r = s.byUs || s.last.want == 0 ? reasonName(s.last.reason) : "manual";
    if (!on && !r) r = "manual";
    if (r) o["off_reason"] = r; else o["off_reason"] = nullptr;
  }
}

void setPriority(const String &id, bool essential) {
  String k = key(id);
  Lock l;
  Sock &s = get(k);
  s.cfg.prio = essential ? Prio::Essential : Prio::Heavy;
  save(k, s);
}

void shedList(JsonArray a) {
  Lock l;
  for (auto &e : g) if (e.second.last.want == 0 && isShedReason(e.second.last.reason)) a.add(e.first);
}
}  // namespace socketcfg
