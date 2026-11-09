#pragma once
#include <Arduino.h>

// v15 HMAC request auth (SW VP spec, 2026-10-23), same for HTTP, OTA and the WS upgrade:
//   X-Device-ID: <serial, or MAC hex until the station writes one>
//   X-Timestamp: <unix s>   X-Nonce: <16 random bytes hex>
//   X-Signature: hex HMAC-SHA256(raw device_secret, METHOD\nPATH?query\nts\nnonce\nhex(sha256(body)))
// Signing needs a trusted clock (NTP or phone); an NVS-restored clock is not used.
#ifndef POWERCELL_LEGACY_SECRET_HEADER
#define POWERCELL_LEGACY_SECRET_HEADER 1  // migration: also send x-device-secret
#endif
namespace devauth {
struct Headers { char id[24]; char ts[12]; char nonce[33]; char sig[65]; };
const char *deviceId();       // header value
bool clockTrusted();
bool canSign();               // secret present and clock trusted
bool sign(const char *method, const char *pathQuery, const uint8_t *body, size_t len, Headers &out);
String pathOf(const String &url);  // "https://host/a?b" -> "/a?b"
void resync();                // restart SNTP (401 with X-Time-Skew)
bool waitSync(uint32_t ms);   // true once a fresh NTP sync landed
}  // namespace devauth
