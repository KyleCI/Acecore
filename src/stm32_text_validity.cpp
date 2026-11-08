#include "stm32_text_validity.h"

#include <ctype.h>
#include <string.h>

namespace loadac {
namespace {
const char *skip(const char *p) {
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}

bool key_is(const char *key, const char *name) {
  while (*name) {
    char c = *key++;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c != *name++) return false;
  }
  return *key == 0;
}

void copy_key(const char *start, const char *end, char *dst, size_t n) {
  size_t j = 0;
  for (const char *p = start; p < end && j + 1 < n; ++p) {
    if (*p == ' ' || *p == '\t') continue;
    char c = *p;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    dst[j++] = c;
  }
  dst[j] = 0;
}

TextMode mode_of(const char *value) {
  value = skip(value);
  if (!strncmp(value, "INVERTER", 8)) return TextMode::Inverter;
  if (!strncmp(value, "GRID", 4)) return TextMode::Grid;
  if (!strncmp(value, "IDLE", 4)) return TextMode::Idle;
  if (!strncmp(value, "FAULT", 5)) return TextMode::Fault;
  if (!strncmp(value, "RESERVE", 7)) return TextMode::Reserve;
  return TextMode::Unknown;
}

TextSwitch switch_of(const char *value) {
  value = skip(value);
  if (!strncmp(value, "INVERTER", 8)) return TextSwitch::Inverter;
  if (!strncmp(value, "GRID", 4)) return TextSwitch::Grid;
  return TextSwitch::Unknown;
}
}  // namespace

TextValidityConfig default_text_limits() {
  TextValidityConfig c;
  c.max_voltage_mv = 400000;
  c.max_current_ma = 100000;
  c.clamp_ambiguous_below_ma = 180;
  c.max_same_record_skew_ms = 1000;
  return c;
}

LineageGate closed_lineage_gate() {
  LineageGate g;
  g.installed_lineage_verified = false;
  g.rms_validity_transmitted = false;
  return g;
}

bool live_estimate_allowed(const LineageGate &gate, const TextCandidate &candidate) {
  return gate.installed_lineage_verified && gate.rms_validity_transmitted && candidate.provisional_inverter_only &&
         !candidate.mutex_failed && !candidate.stale;
}

Stm32TextAssembler::Stm32TextAssembler(TextValidityConfig cfg)
    : cfg_(cfg), open_(false), banner_(false), mutex_failed_(false), mode_(TextMode::Unknown),
      sw_(TextSwitch::Unknown), v_seen_(false), i_seen_(false), v_ok_(false), i_ok_(false), v_mv_(0), i_ma_(0),
      opened_ms_(0), v_ms_(0), i_ms_(0), have_v_ms_(false), have_i_ms_(false) {}

void Stm32TextAssembler::clear_fields() {
  v_seen_ = i_seen_ = v_ok_ = i_ok_ = false;
  have_v_ms_ = have_i_ms_ = false;
  v_mv_ = i_ma_ = 0;
  v_ms_ = i_ms_ = 0;
  sw_ = TextSwitch::Unknown;
  mode_ = TextMode::Unknown;
}

void Stm32TextAssembler::reset_boot() {
  open_ = false;
  mutex_failed_ = false;
  clear_fields();
}

void Stm32TextAssembler::note_mutex_failure(uint32_t mono_ms, TextCandidate *out) {
  mutex_failed_ = true;
  open_ = false;
  clear_fields();
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->closed = true;
  out->mutex_failed = true;
  out->reason = "mutex_failure";
  out->closed_ms = mono_ms;
  out->banner_xfer4_seen = banner_;
}

bool Stm32TextAssembler::take_number(const char *value, int32_t *milli) {
  value = skip(value);
  if (*value == '+') ++value;
  if (!isdigit((unsigned char)*value)) return false;
  int64_t whole = 0;
  while (isdigit((unsigned char)*value)) {
    whole = whole * 10 + (*value - '0');
    if (whole > 1000000) return false;
    ++value;
  }
  int32_t frac = 0;
  int places = 0;
  if (*value == '.') {
    ++value;
    if (!isdigit((unsigned char)*value)) return false;
    while (isdigit((unsigned char)*value)) {
      if (places < 3) frac = frac * 10 + (*value - '0');
      ++places;
      ++value;
    }
    if (places > 3) return false;
    while (places < 3) {
      frac *= 10;
      ++places;
    }
  }
  value = skip(value);
  if (*value == 'V' || *value == 'v' || *value == 'A' || *value == 'a') {
    ++value;
    value = skip(value);
  }
  if (*value != 0) return false;
  *milli = (int32_t)(whole * 1000 + frac);
  return true;
}

bool Stm32TextAssembler::publish(uint32_t mono_ms, TextCandidate *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  out->closed = true;
  out->mode = mode_;
  out->sw = sw_;
  out->v_in_record = v_seen_;
  out->i_in_record = i_seen_;
  out->v_numeric = v_ok_;
  out->i_numeric = i_ok_;
  out->v_mv = v_ok_ ? v_mv_ : 0;
  out->i_ma = i_ok_ ? i_ma_ : 0;
  out->opened_ms = opened_ms_;
  out->closed_ms = mono_ms;
  out->banner_xfer4_seen = banner_;
  out->mutex_failed = mutex_failed_;
  bool skew = false;
  if (have_v_ms_ && have_i_ms_) {
    uint32_t span = v_ms_ > i_ms_ ? v_ms_ - i_ms_ : i_ms_ - v_ms_;
    if (span > cfg_.max_same_record_skew_ms) skew = true;
  }
  if (mono_ms >= opened_ms_ && cfg_.max_same_record_skew_ms && mono_ms - opened_ms_ > cfg_.max_same_record_skew_ms &&
      (v_seen_ || i_seen_))
    skew = true;
  out->stale = skew;
  const char *reason = "absent";
  bool provisional = false;
  if (mutex_failed_) reason = "mutex_failure";
  else if (mode_ != TextMode::Inverter) reason = "not_sys_inverter";
  else if (sw_ == TextSwitch::Grid) reason = "switch_grid";
  else if (!v_seen_ || !i_seen_) reason = "split_record";
  else if (!v_ok_ || !i_ok_) reason = "malformed_number";
  else if (skew) reason = "stale_fields";
  else if (v_mv_ <= 0 || v_mv_ > cfg_.max_voltage_mv) reason = "voltage_range";
  else if (i_ma_ <= 0 || i_ma_ < cfg_.clamp_ambiguous_below_ma) reason = "current_clamp_ambiguous";
  else if (i_ma_ > cfg_.max_current_ma) reason = "current_range";
  else {
    provisional = true;
    reason = "provisional_inverter_only";
  }
  out->provisional_inverter_only = provisional;
  out->live_eligible = false;
  out->reason = reason;
  return true;
}

bool Stm32TextAssembler::feed_line(uint32_t mono_ms, const char *line, TextCandidate *published) {
  if (published) memset(published, 0, sizeof(*published));
  if (!line) return false;
  line = skip(line);
  if (strstr(line, "PC-CALEB-XFER4")) banner_ = true;
  const char *colon = strchr(line, ':');
  if (!colon) return false;
  char key[12];
  copy_key(line, colon, key, sizeof(key));
  const char *value = skip(colon + 1);
  bool closed = false;
  if (key_is(key, "SYS")) {
    if (open_) closed = publish(mono_ms, published);
    clear_fields();
    mutex_failed_ = false;
    open_ = true;
    opened_ms_ = mono_ms;
    mode_ = mode_of(value);
    return closed;
  }
  if (!open_) return false;
  if (key_is(key, "SW")) {
    sw_ = switch_of(value);
    if (sw_ == TextSwitch::Grid) {
      v_seen_ = i_seen_ = v_ok_ = i_ok_ = false;
    }
    return false;
  }
  if (key_is(key, "VOUT")) {
    v_seen_ = true;
    v_ms_ = mono_ms;
    have_v_ms_ = true;
    v_ok_ = take_number(value, &v_mv_);
    if (!v_ok_) v_mv_ = 0;
    return false;
  }
  if (key_is(key, "IOUT")) {
    i_seen_ = true;
    i_ms_ = mono_ms;
    have_i_ms_ = true;
    i_ok_ = take_number(value, &i_ma_);
    if (!i_ok_) i_ma_ = 0;
    return false;
  }
  return false;
}

}  // namespace loadac
