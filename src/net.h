#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
// Wi-Fi (firmware 2.1, spec section 5): states UNCONFIGURED (AP), TESTING (AP_STA),
// ONLINE (STA), FALLBACK (AP_STA). The hotspot "Powercell-XXXXXX" (per-unit password)
// is on with no saved network, during a Wi-Fi test, after AP_FALLBACK_MIN minutes
// without the router, and for AP_FORCE_MS after the console "AP ON". It closes 120 s
// after the station is up (or after /v2/wifi/save). No reboot after saving.
#include "onb_core.h"
namespace net {
// v14 network mode (persisted as pc_secure/net_mode). Unset keeps the legacy behaviour.
pc::onb::NetMode networkMode();
bool setNetworkMode(pc::onb::NetMode m);  // false for Unset or a storage failure
void begin();
void service();
bool staConnected();
bool backendAllowed();  // saved network, no Wi-Fi test running or unsaved
bool apActive();
const char *stateName();
void forceAp();                 // console "AP ON"
void restartAp(uint32_t delayMs);  // hotspot password changed
// Live Wi-Fi test (hotspot stays up). False if a test is already running or input is invalid.
bool startTest(const String &ssid, const String &pass, bool phoneSees5Ghz, String &testId);
void testStatus(JsonDocument &doc);
// Saves the last successful test. False if none. ip = station IP.
bool saveTested(String &ip);
// Cached scan results; starts an async scan when stale. Returns true if results are fresh.
bool scan(JsonArray out);
}  // namespace net
