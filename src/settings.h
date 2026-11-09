#pragma once
#include <Arduino.h>

// Device identity and provisioned settings. Secrets live only in NVS
// (namespace "pc_secure"); this firmware has no compiled-in credentials.
namespace settings {
struct Identity {
  uint8_t mac[6];
  char macColon[18];   // "AA:BB:CC:DD:EE:FF" - WS query and legacy gateway_id
  char deviceId[13];   // "AABBCCDDEEFF"      - X-Device-ID header and device_id fields
  char gatewayName[24];
  char apSsid[24];
};

void begin();
const Identity &id();

// Provisioned values (empty String when missing)
String apiKey();          // backend WS API-KEY
String deviceSecret();    // v17: factory identity secret hex (pc_ident); HMAC key for passcode fingerprint (never sent raw)
String cloudSecret();     // same hex as deviceSecret(); used as x-device-secret / WS HMAC identity
bool validCloudSecret(const String &s);  // exactly 64 hex chars
String otaBaseUrl();      // defaults to POWERCELL_OTA_BASE_URL_DEFAULT
String apPassword();      // printed on the unit label; derived from MAC + eFuse if not provisioned
String localAdminToken(); // setup token printed on the label; valid only on the setup AP (see web_local.h)
String localToken();      // LAN token issued by the backend (user "powercell"); empty until provisioned
bool setLocalToken(const String &token);  // validates format; persists in NVS "pc_secure/local_tok"
String localTokenFingerprint();           // first 16 hex of SHA-256(token), "" if unset (safe to log/ack)
String wifiSsid();
String wifiPass();
bool espnowKeys(uint8_t pmk[16], uint8_t lmk[16], uint8_t auth[16]);
bool provisioningLocked(); // set after the factory step; console then refuses secret changes

bool setString(const char *key, const String &v);
String getString(const char *key);  // raw pc_secure value ("" if missing)
bool setWifi(const String &ssid, const String &pass);
bool clearWifi();
bool setEspnowKeys(const String &hex48);  // 96 hex chars: pmk|lmk|auth
bool lock();
void wipeUser();  // factory reset: remove customer keys from pc_secure
}  // namespace settings
