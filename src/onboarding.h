#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

class AsyncWebServer;

// Firmware 2.1 offline-first onboarding (spec /workspace/esp32_review/onboarding_spec.md).
// NVS: "pc_onb" (passcode verifier, paired phones, owner, limiter). v15: no factory
// passcode; the customer sets the first one in app setup. Factory identity: factory_id.h.
namespace onboarding {
void begin();     // after settings::begin(): creates per-unit random defaults on unlocked units
void service();   // main loop: deferred reboot after factory reset
void registerRoutes(AsyncWebServer &server);  // /v2/* local API (web_v2.cpp)

// status
String displayId();
bool passcodeSet();       // false on legacy field units until a passcode is set
bool passcodeDefault();
bool claimed();
String fingerprint();     // "" if no passcode or no factory identity secret

// local API helpers (thread-safe; called from the AsyncTCP task)
enum class Auth : uint8_t { None, Session, Lan };
struct Caller { Auth auth = Auth::None; String phoneId, phoneName; };
Caller authorize(const String &authHeader);
bool locked(uint32_t ip, uint32_t *retryAfterS);
void authFailed(uint32_t ip);
void authOk(uint32_t ip);
bool matchesDeviceId(const String &id);
bool verifyPasscode(const String &p);  // PBKDF2 check (or the label admin token on legacy units)
// v15 first-time setup: only while no passcode is set (caller checks the hotspot origin).
bool setupPasscode(const String &code, const String &phoneId, const String &phoneName, String &session, String &lanToken);
// Issues a session (30 min) and a LAN token for phoneId. Returns false on storage failure.
bool openSession(const String &phoneId, const String &phoneName, String &session, String &lanToken);
String pair(const String &phoneId, const String &phoneName);  // new LAN token ("" on failure)
void pairsJson(JsonArray out);
bool unpair(const String &phoneId);
// Validated new passcode; revokes every session and LAN token, returns a fresh token for the caller.
bool setPasscode(const String &newCode, const char *origin, const Caller *caller, String &lanToken);
String newClaimCode();     // 8 chars base32; replaces any previous code; TTL 15 min
// v15: as-bought reset (source "button"|"app"|"remote"); reboots shortly after
void factoryReset(const char *source, const String &cmdId = String(), uint32_t issuedAt = 0);

// factory console
String labelLine();                          // "PWC2;S=..;P=..;D=.." ("" after LOCK)

// WebSocket (section 13)
void buildHello(JsonDocument &doc);          // caller adds identity fields
bool helloDue();                             // a fresh device_hello should be sent
void helloSent();
bool takePasscodeChanged(JsonDocument &doc); // pending passcode_changed (caller adds identity)
void handleSetPasscode(JsonDocument &in, JsonDocument &ack, bool tls);
void handleClaimResult(JsonDocument &in);
void handleUnclaim(JsonDocument &in);
void clockSynced();                          // re-report the claim expiry once NTP is set
}  // namespace onboarding
