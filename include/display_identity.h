#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Installed app gate: setup/info device_id is the factory display serial.
// A MAC is a different OTA identity and is not an alias for that serial.
inline bool factory_serial_valid(const char* serial) {
  if (!serial || strlen(serial) != 10 || strncmp(serial, "ACE-", 4) != 0) return false;
  for (int i = 4; i < 10; ++i) {
    if (serial[i] < '0' || serial[i] > '9') return false;
  }
  return true;
}

inline void label_hotspot(const char* serial, char out[24]) {
  out[0] = 0;
  if (!factory_serial_valid(serial)) return;
  snprintf(out, 24, "Powercell-%.6s", serial + 4);
}

inline void fold_identity(const char* in, char* out, size_t cap) {
  size_t n = 0;
  for (; in && *in && n + 1 < cap; ++in) {
    unsigned char c = (unsigned char)*in;
    if (c == ' ' || c == ':' || c == '-' || c == '_') continue;
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    out[n++] = (char)c;
  }
  out[n] = 0;
}

inline bool display_serial_match(const char* requested, const char* serial) {
  if (!factory_serial_valid(serial)) return false;
  char want[32], got[32];
  fold_identity(serial, want, sizeof want);
  fold_identity(requested, got, sizeof got);
  return got[0] && strcmp(want, got) == 0;
}

inline void mac_hex(const uint8_t mac[6], char out[13]) {
  snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
