#include "lora_socket_proto.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace pc {
namespace lora {
namespace {
// Copy the value after `key` up to the next comma into out. Returns false if key absent.
bool field(const char *s, const char *key, char *out, size_t cap) {
  const char *p = strstr(s, key);
  if (!p) return false;
  p += strlen(key);
  size_t i = 0;
  while (*p && *p != ',' && i + 1 < cap) out[i++] = *p++;
  out[i] = 0;
  return *p == 0 || *p == ',';  // too long = reject
}
}  // namespace

bool validCommandId(const char *id) {
  if (!id) return false;
  size_t n = strlen(id);
  if (!n || n > 47) return false;
  for (size_t i = 0; i < n; ++i) if (!isalnum((unsigned char)id[i]) && id[i] != '-') return false;
  return true;
}

bool parseSocketPacket(const char *in, size_t n, SocketPacket &out) {
  out = SocketPacket();
  char s[200];
  if (!in || n == 0 || n >= sizeof s) return false;
  memcpy(s, in, n); s[n] = 0;
  for (size_t i = 0; i < n; ++i) if (s[i] == 0 || (unsigned char)s[i] < 0x20) s[i] = ' ';
  while (n && s[n - 1] == ' ') s[--n] = 0;
  const char *comma = strchr(s, ',');
  if (!comma || comma - s != 12) return false;
  for (int i = 0; i < 12; ++i) { if (!isxdigit((unsigned char)s[i])) return false; out.id[i] = (char)toupper((unsigned char)s[i]); }
  const char *rest = comma + 1;
  char v[48];
  if (field(rest, "SOCKET CURRENT:", v, sizeof v)) {
    char *e = nullptr; float f = strtof(v, &e);
    if (e == v || !isfinite(f) || f < 0 || f > 1000) return false;
    out.current = f; out.hasCurrent = true;
  }
  if (field(rest, "SOCKET RELAY:", v, sizeof v)) {
    if (strcmp(v, "0") && strcmp(v, "1")) return false;
    out.relay = v[0] - '0'; out.hasRelay = true;
  }
  if (!field(rest, "FW:", out.fw, sizeof out.fw)) out.fw[0] = 0;
  if (field(rest, "ACK:", out.ack, sizeof out.ack) && !validCommandId(out.ack)) out.ack[0] = 0;
  out.overcurrent = strstr(rest, "ALERT:OVERCURRENT") != nullptr;
  out.warning = strstr(rest, "WARN:CURRENT_HIGH") != nullptr;
  out.lockout = strstr(rest, "ALERT:LOCKOUT") != nullptr;
  if (field(rest, "TRIP CURRENT:", v, sizeof v)) {
    char *e = nullptr; float f = strtof(v, &e);
    if (e != v && isfinite(f) && f >= 0 && f <= 1000) { out.tripCurrent = f; out.hasTripCurrent = true; }
  }
  if (field(rest, "LIM:", v, sizeof v)) {
    char *e = nullptr; long l = strtol(v, &e, 10);
    if (e != v && !*e && l >= 10 && l <= 100) { out.limitDa = (uint8_t)l; out.hasLimit = true; }
  }
  return out.hasCurrent && out.hasRelay;
}

namespace {
size_t buildCommand(const char *id12, const char *verb, const char *cmdId, char *buf, size_t cap) {
  if (!id12 || strlen(id12) != 12 || !validCommandId(cmdId)) return 0;
  for (int i = 0; i < 12; ++i) if (!isxdigit((unsigned char)id12[i])) return 0;
  int w = snprintf(buf, cap, "%s,%s,%s", id12, verb, cmdId);
  return (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
}
}  // namespace
size_t buildRelayCommand(const char *id12, bool on, const char *cmdId, char *buf, size_t cap) {
  return buildCommand(id12, on ? "RELAY_ON" : "RELAY_OFF", cmdId, buf, cap);
}
size_t buildResetLockoutCommand(const char *id12, const char *cmdId, char *buf, size_t cap) {
  return buildCommand(id12, "RESET_LOCKOUT", cmdId, buf, cap);
}
size_t buildSetLimitCommand(const char *id12, uint8_t da, const char *cmdId, char *buf, size_t cap) {
  char verb[24]; snprintf(verb, sizeof verb, "SET_LIMIT,%u", (unsigned)(da < 10 ? 10 : da > 100 ? 100 : da));
  return buildCommand(id12, verb, cmdId, buf, cap);
}
}  // namespace lora
}  // namespace pc
