#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pc {
// Canonical id formats derived from the Wi-Fi STA MAC.
void macColon(const uint8_t mac[6], char out[18]);  // "AA:BB:CC:DD:EE:FF" (WS query, gateway_id - legacy)
void macHex(const uint8_t mac[6], char out[13]);    // "AABBCCDDEEFF"      (X-Device-ID, device_id, OTA)
// Default gateway name "ACE-" + last 3 MAC bytes, e.g. "ACE-DDEEFF". Unique per unit.
void gatewayName(const uint8_t mac[6], char out[16]);
// Setup AP SSID "Powercell-DDEEFF".
void apSsid(const uint8_t mac[6], char out[24]);
// Per-device fallback AP password: 10 chars from HMAC-free mix of the MAC and
// a per-device secret salt (eFuse-derived on target). Prefer a password
// provisioned in NVS and printed on the unit label; this is only the
// fallback when none was provisioned. Alphabet avoids look-alikes.
void apPassword(const uint8_t mac[6], const uint8_t *salt, size_t saltLen, char out[11]);
// URL-encode for query strings (RFC 3986 unreserved kept).
size_t urlEncode(const char *in, char *out, size_t cap);
}  // namespace pc
