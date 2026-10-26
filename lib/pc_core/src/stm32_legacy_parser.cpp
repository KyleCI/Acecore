#include "stm32_legacy_parser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace pc {
namespace {
const char *skipWs(const char *p) { while (*p == ' ' || *p == '\t') ++p; return p; }
bool startsWith(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }
// Parse first float in s; returns false if none.
bool firstFloat(const char *s, float &v, const char **end = nullptr) {
  while (*s && !(isdigit((unsigned char)*s) || ((*s == '-' || *s == '.') && isdigit((unsigned char)s[1])))) ++s;
  if (!*s) return false;
  char *e = nullptr;
  v = strtof(s, &e);
  if (e == s) return false;
  if (end) *end = e;
  return true;
}
}  // namespace

const char *sysStateName(SysState s) {
  switch (s) { case SysState::Idle: return "IDLE"; case SysState::Grid: return "GRID";
    case SysState::Inverter: return "INVERTER"; case SysState::Fault: return "FAULT"; case SysState::Reserve: return "RESERVE"; default: return "UNKNOWN"; }
}
const char *switchName(SwitchPos s) {
  switch (s) { case SwitchPos::Inverter: return "INVERTER"; case SwitchPos::Grid: return "GRID"; default: return "UNKNOWN"; }
}
const char *chargeName(ChargeState s) {
  switch (s) { case ChargeState::Off: return "OFF"; case ChargeState::Idle: return "IDLE"; case ChargeState::Wait: return "WAIT";
    case ChargeState::Bulk: return "BULK"; case ChargeState::Absorb: return "ABSORB"; case ChargeState::Float: return "FLOAT";
    case ChargeState::Fault: return "FAULT"; default: return "UNKNOWN"; }
}

void LegacyTextParser::reset() { flags_ = 0; len_ = 0; overflow_ = false; inRecord_ = false; work_ = Stm32Status(); }

bool LegacyTextParser::publish(Stm32Status &out) {
  if (!inRecord_) return false;
  if (work_.sys != SysState::Fault && work_.sys != SysState::Unknown) flags_ &= ~((1u << 17) | (1u << 18));
  if (work_.chr == ChargeState::Bulk || work_.chr == ChargeState::Absorb || work_.chr == ChargeState::Off || work_.chr == ChargeState::Fault)
    flags_ &= ~(1u << 19);
  work_.fault = ((work_.sys == SysState::Fault) ? lastFault_ : 0) | flags_;
  if (work_.sys != SysState::Fault) lastFault_ = 0;
  work_.seq = ++seq_;
  out = work_;
  inRecord_ = false;
  work_ = Stm32Status();
  return true;
}

bool LegacyTextParser::feed(uint8_t c, Stm32Status &out) {
  if (c == '\r') return false;
  if (c == '\n') {
    bool published = false;
    if (overflow_) ++overflows_;
    else if (len_) { line_[len_] = 0; ++lines_; published = handleLine(out); }
    len_ = 0; overflow_ = false;
    return published;
  }
  if (overflow_) return false;
  if (len_ >= kMaxLine) { overflow_ = true; return false; }
  line_[len_++] = static_cast<char>(c);
  return false;
}

bool LegacyTextParser::flushIdle(Stm32Status &out) {
  len_ = 0; overflow_ = false;
  return publish(out);
}

bool LegacyTextParser::isReplyLine(const char *s) {
  auto word = [s](const char *w, size_t n) { return !strncmp(s, w, n) && (s[n] == 0 || s[n] == ' '); };
  return word("VER", 3) || word("OK", 2) || word("ERR", 3);
}

bool LegacyTextParser::handleLine(Stm32Status &out) {
  const char *s = skipWs(line_);
  bool published = false;
  if (isReplyLine(s)) {  // command reply: route to the reply slot, not the record
    strncpy(reply_, s, kMaxLine); reply_[kMaxLine] = 0;
    ++replySeq_;
    return false;
  }
  if (startsWith(s, "--- Status")) {
    published = publish(out);  // previous record is complete
    inRecord_ = true;
    return published;
  }
  if (startsWith(s, "SYS")) {
    // A SYS line opens a record unless it directly follows a "--- Status" header.
    if (inRecord_ && work_.sys != SysState::Unknown) published = publish(out);
    inRecord_ = true;
  }
  // Fault banner can appear outside a record: ">>> FAULT: 0x21 <<<"
  if (strstr(s, "FAULT: Permanent fault")) { flags_ |= 1u << 17; return published; }
  if (strstr(s, "FAULT: Max restart attempts")) { flags_ |= 1u << 18; return published; }
  if (strstr(s, "CHR: Battery confirmed full")) { flags_ |= 1u << 19; return published; }
  if (strstr(s, "CHR: FAULT")) flags_ &= ~(1u << 19);
  if (startsWith(s, "WARN:") || startsWith(s, "GRID: Lost") || startsWith(s, "GRID: Restored") ||
      (startsWith(s, "FAULT:") && !strstr(s, "0x"))) return published;  // event lines: not record fields
  const char *f = strstr(s, "FAULT: 0x");
  if (f) { lastFault_ = static_cast<uint32_t>(strtoul(f + 9, nullptr, 16)); return published; }

  // "RSV:1" / "RSV: 0", on its own line or as a token on another record line.
  if (inRecord_) {
    const char *r = strstr(s, "RSV");
    if (r && (r == s || r[-1] == ' ' || r[-1] == '\t')) {
      const char *q = skipWs(r + 3);
      if (*q == ':') { q = skipWs(q + 1); if (*q == '0' || *q == '1') { work_.has_rsv = true; work_.rsv_latched = *q == '1'; } }
    }
  }
  const char *colon = strchr(s, ':');
  if (!colon || !inRecord_) return published;
  char key[8] = {};
  size_t k = 0;
  for (const char *p = s; p < colon && k < sizeof(key) - 1; ++p) if (*p != ' ' && *p != '\t') key[k++] = *p;
  const char *val = skipWs(colon + 1);
  float v = 0;

  if (!strcmp(key, "SYS")) {
    if (startsWith(val, "IDLE")) work_.sys = SysState::Idle;
    else if (startsWith(val, "GRID")) work_.sys = SysState::Grid;
    else if (startsWith(val, "INVERTER")) work_.sys = SysState::Inverter;
    else if (startsWith(val, "FAULT")) work_.sys = SysState::Fault;
    else if (startsWith(val, "RESERVE")) { work_.sys = SysState::Reserve; work_.has_rsv = true; work_.rsv_latched = true; }
  } else if (!strcmp(key, "SW")) {
    if (startsWith(val, "INVERTER")) work_.sw = SwitchPos::Inverter;
    else if (startsWith(val, "GRID")) work_.sw = SwitchPos::Grid;
  } else if (!strcmp(key, "BAT")) {
    const char *end = nullptr;
    if (firstFloat(val, v, &end)) { work_.bat_v = v; work_.has_bat = true;
      float pct;
      if (end && strchr(end, '%') && firstFloat(end, pct) && pct >= 0 && pct <= 100) { work_.bat_pct = (uint8_t)(pct + 0.5f); work_.has_bat_pct = true; }
    }
  } else if (!strcmp(key, "GRID")) {
    if (firstFloat(val, v)) { work_.grid_v = v; work_.has_grid = true; }
  } else if (!strcmp(key, "TEMP")) {
    if (firstFloat(val, v)) { work_.temp_c = v; work_.has_temp = true; }
  } else if (!strcmp(key, "CHR")) {
    if (startsWith(val, "--")) work_.chr = ChargeState::Off;
    else if (startsWith(val, "IDLE")) work_.chr = ChargeState::Idle;
    else if (startsWith(val, "WAIT")) work_.chr = ChargeState::Wait;
    else if (startsWith(val, "BULK")) work_.chr = ChargeState::Bulk;
    else if (startsWith(val, "ABSORB")) work_.chr = ChargeState::Absorb;
    else if (startsWith(val, "FLOAT")) work_.chr = ChargeState::Float;
    else if (startsWith(val, "FAULT")) work_.chr = ChargeState::Fault;
  } else if (!strcmp(key, "VOUT")) {
    if (firstFloat(val, v)) { work_.vout = v; work_.has_vout = true; }
    const char *i = strstr(val, "IOUT");  // "VOUT: 230V  IOUT: 1.2A" on one line
    if (i && firstFloat(i + 4, v)) { work_.iout = v; work_.has_iout = true; }
  } else if (!strcmp(key, "IOUT")) {
    if (firstFloat(val, v)) { work_.iout = v; work_.has_iout = true; }
  }
  return published;
}
}  // namespace pc
