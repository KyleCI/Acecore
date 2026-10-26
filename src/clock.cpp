#include "prefs_safe.h"
#include "clock.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>
#include "log.h"

namespace clock_ {
namespace {
using namespace pc::clk;
const char *NS = "clock";
volatile Source g_src = Source::None;
volatile uint32_t g_ntpAt = 0; volatile bool g_ntpEver = false;
int g_tz = kTzDefault;
uint32_t g_lastSave = 0;
int64_t nowS() { return (int64_t)time(nullptr); }
void setNow(int64_t e) { timeval tv{(time_t)e, 0}; settimeofday(&tv, nullptr); }
void save() {
  int64_t n = nowS();
  if (n < kMinEpoch || (g_src != Source::Ntp && g_src != Source::Phone)) return;  // only good time
  Preferences p; if (p.begin(NS, false)) { p.putLong64("last", n); p.end(); }
}
void onNtp(struct timeval *) { g_src = Source::Ntp; g_ntpAt = millis(); g_ntpEver = true; }
bool ntpFresh() { return g_ntpEver && millis() - g_ntpAt < kNtpFreshMs; }
}  // namespace

void begin() {
  sntp_set_time_sync_notification_cb(onNtp);
  Preferences p;
  int64_t last = 0;
  if (prefsOpenRO(p, NS)) { g_tz = p.getInt("tz", kTzDefault); last = p.getLong64("last", 0); p.end(); }
  if (!validTz(g_tz)) g_tz = kTzDefault;
  if (nowS() < kMinEpoch && last >= kMinEpoch) {
    setNow(last); g_src = Source::Nvs;
    LOGI("CLOCK", "NVS_RESTORE", "last good time %lld (stale by the downtime)", (long long)last);
  }
}

void service() {
  uint32_t m = millis();
  if (g_lastSave && m - g_lastSave < 3600000UL) return;
  if (nowS() < kMinEpoch || (g_src != Source::Ntp && g_src != Source::Phone)) return;
  g_lastSave = m ? m : 1; save();
}

Verdict setFromPhone(int64_t epoch, bool hasTz, long tz) {
  if (hasTz && validTz(tz) && tz != g_tz) {
    g_tz = (int)tz; Preferences p; if (p.begin(NS, false)) { p.putInt("tz", g_tz); p.end(); }
  }
  Verdict v = judge(epoch, nowS(), g_src, ntpFresh());
  if (v == Verdict::Accept) {
    LOGI("CLOCK", "PHONE_SET", "epoch=%lld was=%lld src=%s", (long long)epoch, (long long)nowS(), sourceName(g_src));
    setNow(epoch); g_src = Source::Phone; save(); g_lastSave = millis();
  }
  return v;
}
int tzOffsetMin() { return g_tz; }
uint32_t ntpAtMs() { return g_ntpEver ? (g_ntpAt ? g_ntpAt : 1) : 0; }
void clearUserSettings() { Preferences p; if (p.begin(NS, false)) { if (p.isKey("tz")) p.remove("tz"); p.end(); } }
Source source() { return g_src; }
void toJson(JsonObject o) {
  int64_t n = nowS();
  if (n >= kMinEpoch) o["epoch"] = n; else o["epoch"] = nullptr;
  o["tz_offset_min"] = g_tz;
  o["source"] = sourceName(g_src);
  o["ntp_fresh"] = ntpFresh();
}
}  // namespace clock_
