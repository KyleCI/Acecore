#include "mdns_core.h"
#include <stdio.h>
#include <string.h>

namespace pc {
namespace mdns {
namespace {
bool labelChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}
}  // namespace

size_t hostLabel(const char *in, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = 0;
  if (!in) return 0;
  const size_t lim = cap - 1 < kMaxLabel ? cap - 1 : kMaxLabel;
  size_t n = 0;
  bool dash = false;  // a '-' is owed before the next kept char
  for (const char *p = in; *p && n < lim; ++p) {
    char c = *p;
    if (labelChar(c) && c != '-') {
      if (dash && n > 0) {
        out[n++] = '-';
        if (n >= lim) break;
      }
      dash = false;
      out[n++] = c;
    } else {
      dash = true;  // '-' itself and any other byte: collapse into one '-'
    }
  }
  while (n > 0 && out[n - 1] == '-') --n;
  out[n] = 0;
  return n;
}

size_t hostname(const char *displayId, const uint8_t mac[6], char *out, size_t cap) {
  size_t n = hostLabel(displayId, out, cap);
  if (n || !mac || !out || cap == 0) return n;
  snprintf(out, cap, "ACE-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return strlen(out);
}

Action Announcer::decide(bool allowed, uint32_t gen, const char *want, uint32_t now) const {
  if (!allowed || !want || !*want) return running ? Action::Stop : Action::None;
  if (!running) {
    if (failed && gen == seenGen && now - failAt < kRetryMs) return Action::None;
    return Action::Start;
  }
  if (gen != seenGen || strcmp(host, want) != 0) return Action::Restart;
  return Action::None;
}

void Announcer::started(bool ok, const char *h, uint32_t gen, uint32_t now) {
  running = ok;
  failed = !ok;
  seenGen = gen;
  failAt = ok ? 0 : now;
  if (ok && h) {
    strncpy(host, h, sizeof host - 1);
    host[sizeof host - 1] = 0;
  } else {
    host[0] = 0;
  }
}

void Announcer::stopped() {
  running = false;
  failed = false;
  host[0] = 0;
}
}  // namespace mdns
}  // namespace pc
