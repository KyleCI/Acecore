#include "load_ac_energy_upload.h"

#include <stdio.h>
#include <string.h>

namespace loadac {
namespace {

struct Scan {
  const char *s;
  uint32_t i;
  uint32_t n;
};

void skip_ws(Scan *sc) {
  while (sc->i < sc->n) {
    char c = sc->s[sc->i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    sc->i++;
  }
}

bool read_string(Scan *sc, char *dst, uint32_t cap, bool *too_long) {
  if (sc->i >= sc->n || sc->s[sc->i] != '"') return false;
  sc->i++;
  uint32_t k = 0;
  while (sc->i < sc->n) {
    unsigned char c = (unsigned char)sc->s[sc->i++];
    if (c == '"') {
      if (dst && cap) dst[k < cap ? k : cap - 1] = 0;
      if (too_long && cap && k >= cap) *too_long = true;
      return true;
    }
    if (c < 32 || c == '\\') return false;
    if (dst && k + 1 < cap) dst[k] = (char)c;
    k++;
  }
  return false;
}

bool skip_string(Scan *sc) { return read_string(sc, 0, 0, 0); }

bool read_uint(Scan *sc, bool *integral, bool *fits, uint32_t *out) {
  if (sc->i >= sc->n || sc->s[sc->i] < '0' || sc->s[sc->i] > '9') return false;
  if (sc->s[sc->i] == '0' && sc->i + 1 < sc->n && sc->s[sc->i + 1] >= '0' && sc->s[sc->i + 1] <= '9') return false;
  uint32_t v = 0;
  bool ok_fit = true;
  while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
    uint32_t d = (uint32_t)(sc->s[sc->i] - '0');
    if (v > (0xFFFFFFFFu - d) / 10u) ok_fit = false;
    else v = v * 10u + d;
    sc->i++;
  }
  if (sc->i < sc->n && (sc->s[sc->i] == '.' || sc->s[sc->i] == 'e' || sc->s[sc->i] == 'E' || sc->s[sc->i] == '+' ||
                        sc->s[sc->i] == '-')) {
    *integral = false;
    while (sc->i < sc->n) {
      char c = sc->s[sc->i];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') sc->i++;
      else break;
    }
    return true;
  }
  *integral = true;
  *fits = ok_fit;
  if (ok_fit) *out = v;
  return true;
}

bool skip_container(Scan *sc, char open, char close) {
  if (sc->i >= sc->n || sc->s[sc->i] != open) return false;
  int depth = 1;
  sc->i++;
  while (sc->i < sc->n && depth > 0) {
    char c = sc->s[sc->i];
    if (c == '"') {
      if (!skip_string(sc)) return false;
      continue;
    }
    sc->i++;
    if (c == open) depth++;
    else if (c == close) depth--;
  }
  return depth == 0;
}

bool skip_value(Scan *sc) {
  skip_ws(sc);
  if (sc->i >= sc->n) return false;
  char c = sc->s[sc->i];
  if (c == '"') return skip_string(sc);
  if (c == '{') return skip_container(sc, '{', '}');
  if (c == '[') return skip_container(sc, '[', ']');
  if (c == 't') {
    if (sc->i + 4 <= sc->n && memcmp(sc->s + sc->i, "true", 4) == 0) {
      sc->i += 4;
      return true;
    }
    return false;
  }
  if (c == 'f') {
    if (sc->i + 5 <= sc->n && memcmp(sc->s + sc->i, "false", 5) == 0) {
      sc->i += 5;
      return true;
    }
    return false;
  }
  if (c == 'n') {
    if (sc->i + 4 <= sc->n && memcmp(sc->s + sc->i, "null", 4) == 0) {
      sc->i += 4;
      return true;
    }
    return false;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    if (c == '-') sc->i++;
    bool integral = false, fits = false;
    uint32_t ignored = 0;
    return read_uint(sc, &integral, &fits, &ignored);
  }
  return false;
}

enum class ValueKind : uint8_t { Bad = 0, String, Bool, Int, Other };

struct Value {
  ValueKind kind;
  bool boolean;
  bool fits;
  uint32_t integer;
  bool too_long;
  char text[181];
};

bool read_value(Scan *sc, Value *out) {
  memset(out, 0, sizeof(*out));
  skip_ws(sc);
  if (sc->i >= sc->n) return false;
  char c = sc->s[sc->i];
  if (c == '"') {
    out->kind = ValueKind::String;
    if (!read_string(sc, out->text, sizeof(out->text), &out->too_long)) return false;
    return true;
  }
  if (c == 't' || c == 'f') {
    out->kind = ValueKind::Bool;
    if (c == 't') {
      if (sc->i + 4 > sc->n || memcmp(sc->s + sc->i, "true", 4) != 0) return false;
      sc->i += 4;
      out->boolean = true;
    } else {
      if (sc->i + 5 > sc->n || memcmp(sc->s + sc->i, "false", 5) != 0) return false;
      sc->i += 5;
      out->boolean = false;
    }
    return true;
  }
  if (c >= '0' && c <= '9') {
    bool integral = false;
    if (!read_uint(sc, &integral, &out->fits, &out->integer)) return false;
    out->kind = integral ? ValueKind::Int : ValueKind::Other;
    return true;
  }
  out->kind = ValueKind::Other;
  return skip_value(sc);
}

struct Fields {
  bool malformed;
  bool extra;
  bool duplicate;
  bool status_success;
  bool status_error;
  bool ack_true;
  bool ack_false;
  bool replay_set;
  bool replay;
  bool simulation_set;
  bool simulation;
  bool record_set;
  uint32_t record_id;
  bool clock_set;
  uint32_t clock_version;
  bool contract_ok;
  bool stream_ok;
  bool device_set;
  bool epoch_set;
  bool boot_set;
  bool seq_set;
  bool error_set;
  bool message_set;
  char device_id[121];
  char meter_epoch[24];
  char boot_id[65];
  char seq[24];
  char error[40];
};

bool token_ok(const char *s, uint32_t limit) {
  if (!s || !s[0]) return false;
  uint32_t n = 0;
  for (const char *p = s; *p; ++p, ++n) {
    if (n >= limit) return false;
    unsigned char c = (unsigned char)*p;
    if (c < 32 || c == 127) return false;
  }
  return n >= 1 && n <= limit;
}

bool uint_text(const char *s, uint64_t *out) {
  if (!s || !s[0]) return false;
  if (s[0] == '0' && s[1]) return false;
  uint64_t v = 0;
  for (const char *p = s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    uint64_t d = (uint64_t)(*p - '0');
    if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10ull) return false;
    v = v * 10ull + d;
  }
  *out = v;
  return true;
}

const char *kErrors[] = {
    "conflicting_duplicate",
    "counter_discontinuity",
    "monotonic_overlap",
    "out_of_order",
    "utc_overlap",
    "clock_correction_changed_energy",
    "model_pf",
    "device_mismatch",
    "unsupported_stream",
    "unsupported_contract",
    "energy_formula",
    "decimal_string",
    "sample_validity",
    "measurement_unknown",
    "model",
    "rejected",
    "unavailable",
    0};

bool known_error(const char *code) {
  for (int i = 0; kErrors[i]; ++i) {
    if (strcmp(kErrors[i], code) == 0) return true;
  }
  return false;
}

bool parse_fields(const char *json, Fields *f) {
  memset(f, 0, sizeof(*f));
  if (!json) {
    f->malformed = true;
    return false;
  }
  Scan sc = {json, 0, (uint32_t)strlen(json)};
  skip_ws(&sc);
  if (sc.i >= sc.n || sc.s[sc.i] != '{') {
    f->malformed = true;
    return false;
  }
  sc.i++;
  skip_ws(&sc);
  if (sc.i < sc.n && sc.s[sc.i] == '}') {
    sc.i++;
    skip_ws(&sc);
    if (sc.i != sc.n) f->malformed = true;
    return !f->malformed;
  }
  while (sc.i < sc.n) {
    skip_ws(&sc);
    char key[40];
    bool key_long = false;
    if (!read_string(&sc, key, sizeof(key), &key_long)) {
      f->malformed = true;
      return false;
    }
    skip_ws(&sc);
    if (sc.i >= sc.n || sc.s[sc.i] != ':') {
      f->malformed = true;
      return false;
    }
    sc.i++;
    Value value;
    if (!read_value(&sc, &value)) {
      f->malformed = true;
      return false;
    }
    bool known = true;
    if (key_long) {
      f->extra = true;
      known = false;
    } else if (strcmp(key, "status") == 0) {
      if (f->status_success || f->status_error) f->duplicate = true;
      if (value.kind == ValueKind::String && strcmp(value.text, "success") == 0) f->status_success = true;
      else if (value.kind == ValueKind::String && strcmp(value.text, "error") == 0) f->status_error = true;
      else f->extra = true;
    } else if (strcmp(key, "ack") == 0) {
      if (f->ack_true || f->ack_false) f->duplicate = true;
      if (value.kind == ValueKind::Bool && value.boolean) f->ack_true = true;
      else if (value.kind == ValueKind::Bool && !value.boolean) f->ack_false = true;
      else f->extra = true;
    } else if (strcmp(key, "replay") == 0) {
      if (f->replay_set) f->duplicate = true;
      if (value.kind == ValueKind::Bool) {
        f->replay_set = true;
        f->replay = value.boolean;
      } else f->extra = true;
    } else if (strcmp(key, "simulation") == 0) {
      if (f->simulation_set) f->duplicate = true;
      if (value.kind == ValueKind::Bool) {
        f->simulation_set = true;
        f->simulation = value.boolean;
      } else f->extra = true;
    } else if (strcmp(key, "record_id") == 0) {
      if (f->record_set) f->duplicate = true;
      if (value.kind == ValueKind::Int && value.fits && value.integer >= 1) {
        f->record_set = true;
        f->record_id = value.integer;
      } else f->extra = true;
    } else if (strcmp(key, "clock_correction_version") == 0) {
      if (f->clock_set) f->duplicate = true;
      if (value.kind == ValueKind::Int && value.fits && value.integer <= 1000000u) {
        f->clock_set = true;
        f->clock_version = value.integer;
      } else f->extra = true;
    } else if (strcmp(key, "contract") == 0) {
      if (f->contract_ok) f->duplicate = true;
      f->contract_ok = value.kind == ValueKind::String && strcmp(value.text, kContract) == 0 && !value.too_long;
      if (!f->contract_ok) f->extra = true;
    } else if (strcmp(key, "stream") == 0) {
      if (f->stream_ok) f->duplicate = true;
      f->stream_ok = value.kind == ValueKind::String && strcmp(value.text, kStream) == 0 && !value.too_long;
      if (!f->stream_ok) f->extra = true;
    } else if (strcmp(key, "device_id") == 0) {
      if (f->device_set) f->duplicate = true;
      if (value.kind == ValueKind::String && !value.too_long && token_ok(value.text, 120)) {
        f->device_set = true;
        memcpy(f->device_id, value.text, sizeof(f->device_id));
      } else f->extra = true;
    } else if (strcmp(key, "meter_epoch") == 0) {
      if (f->epoch_set) f->duplicate = true;
      if (value.kind == ValueKind::String && !value.too_long && token_ok(value.text, 20)) {
        f->epoch_set = true;
        memcpy(f->meter_epoch, value.text, sizeof(f->meter_epoch));
      } else f->extra = true;
    } else if (strcmp(key, "boot_id") == 0) {
      if (f->boot_set) f->duplicate = true;
      if (value.kind == ValueKind::String && !value.too_long && token_ok(value.text, 64)) {
        f->boot_set = true;
        memcpy(f->boot_id, value.text, sizeof(f->boot_id));
      } else f->extra = true;
    } else if (strcmp(key, "seq") == 0) {
      if (f->seq_set) f->duplicate = true;
      if (value.kind == ValueKind::String && !value.too_long && token_ok(value.text, 20)) {
        f->seq_set = true;
        memcpy(f->seq, value.text, sizeof(f->seq));
      } else f->extra = true;
    } else if (strcmp(key, "error") == 0) {
      if (f->error_set) f->duplicate = true;
      if (value.kind == ValueKind::String && !value.too_long && known_error(value.text)) {
        f->error_set = true;
        memcpy(f->error, value.text, sizeof(f->error));
      } else f->extra = true;
    } else if (strcmp(key, "message") == 0) {
      if (f->message_set) f->duplicate = true;
      uint32_t n = 0;
      while (value.text[n]) n++;
      if (value.kind == ValueKind::String && !value.too_long && n <= 180) f->message_set = true;
      else f->extra = true;
    } else {
      known = false;
      f->extra = true;
    }
    (void)known;
    skip_ws(&sc);
    if (sc.i < sc.n && sc.s[sc.i] == ',') {
      sc.i++;
      continue;
    }
    if (sc.i < sc.n && sc.s[sc.i] == '}') {
      sc.i++;
      skip_ws(&sc);
      if (sc.i != sc.n) f->malformed = true;
      return !f->malformed;
    }
    f->malformed = true;
    return false;
  }
  f->malformed = true;
  return false;
}

enum class FrameKind : uint8_t { Unrelated = 0, Malformed, NotDurable, Negative, Durable };

FrameKind classify(const Fields &f) {
  if (f.malformed) return FrameKind::Malformed;
  if (!f.ack_true && !f.ack_false) return FrameKind::Unrelated;
  if (f.duplicate || f.extra) return FrameKind::NotDurable;
  if (f.ack_true && f.status_success && f.replay_set && f.simulation_set && f.record_set && f.clock_set && f.contract_ok &&
      f.stream_ok && f.device_set && f.epoch_set && f.boot_set && f.seq_set && !f.error_set && !f.message_set) {
    return FrameKind::Durable;
  }
  if (f.ack_false && f.status_error && f.error_set && f.message_set && f.contract_ok && f.stream_ok && !f.record_set &&
      !f.replay_set && !f.simulation_set) {
    return FrameKind::Negative;
  }
  return FrameKind::NotDurable;
}

struct Identity {
  bool ok;
  bool simulation;
  uint32_t clock_version;
  uint32_t boot_u32;
  uint64_t seq_u64;
  char device_id[121];
  char meter_epoch[24];
  char boot_id[65];
  char seq[24];
};

bool parse_identity(const char *json, Identity *id) {
  memset(id, 0, sizeof(*id));
  Scan sc = {json, 0, (uint32_t)strlen(json)};
  skip_ws(&sc);
  if (sc.i >= sc.n || sc.s[sc.i] != '{') return false;
  sc.i++;
  bool contract = false, stream = false, simulation = false, clock = false;
  bool device = false, epoch = false, boot = false, seq = false;
  while (sc.i < sc.n) {
    skip_ws(&sc);
    if (sc.i < sc.n && sc.s[sc.i] == '}') {
      sc.i++;
      break;
    }
    char key[40];
    bool key_long = false;
    if (!read_string(&sc, key, sizeof(key), &key_long)) return false;
    skip_ws(&sc);
    if (sc.i >= sc.n || sc.s[sc.i] != ':') return false;
    sc.i++;
    if (key_long) {
      if (!skip_value(&sc)) return false;
    } else if (strcmp(key, "contract") == 0 || strcmp(key, "stream") == 0 || strcmp(key, "device_id") == 0 ||
               strcmp(key, "meter_epoch") == 0 || strcmp(key, "boot_id") == 0 || strcmp(key, "seq") == 0 ||
               strcmp(key, "simulation") == 0 || strcmp(key, "clock_correction_version") == 0) {
      Value value;
      if (!read_value(&sc, &value)) return false;
      if (strcmp(key, "contract") == 0)
        contract = value.kind == ValueKind::String && strcmp(value.text, kContract) == 0;
      else if (strcmp(key, "stream") == 0)
        stream = value.kind == ValueKind::String && strcmp(value.text, kStream) == 0;
      else if (strcmp(key, "simulation") == 0 && value.kind == ValueKind::Bool) {
        simulation = true;
        id->simulation = value.boolean;
      } else if (strcmp(key, "clock_correction_version") == 0 && value.kind == ValueKind::Int && value.fits &&
                 value.integer <= 1000000u) {
        clock = true;
        id->clock_version = value.integer;
      } else if (strcmp(key, "device_id") == 0 && value.kind == ValueKind::String && token_ok(value.text, 120)) {
        device = true;
        memcpy(id->device_id, value.text, sizeof(id->device_id));
      } else if (strcmp(key, "meter_epoch") == 0 && value.kind == ValueKind::String && token_ok(value.text, 20)) {
        epoch = true;
        memcpy(id->meter_epoch, value.text, sizeof(id->meter_epoch));
      } else if (strcmp(key, "boot_id") == 0 && value.kind == ValueKind::String && token_ok(value.text, 64)) {
        boot = true;
        memcpy(id->boot_id, value.text, sizeof(id->boot_id));
      } else if (strcmp(key, "seq") == 0 && value.kind == ValueKind::String && token_ok(value.text, 20)) {
        seq = true;
        memcpy(id->seq, value.text, sizeof(id->seq));
      }
    } else if (!skip_value(&sc)) {
      return false;
    }
    skip_ws(&sc);
    if (sc.i < sc.n && sc.s[sc.i] == ',') {
      sc.i++;
      continue;
    }
    if (sc.i < sc.n && sc.s[sc.i] == '}') {
      sc.i++;
      break;
    }
    return false;
  }
  uint64_t boot_u = 0, seq_u = 0;
  if (!contract || !stream || !simulation || !clock || !device || !epoch || !boot || !seq) return false;
  if (!uint_text(id->meter_epoch, &boot_u)) return false;
  (void)boot_u;
  if (!uint_text(id->boot_id, &boot_u) || boot_u > 0xFFFFFFFFull) return false;
  if (!uint_text(id->seq, &seq_u)) return false;
  id->boot_u32 = (uint32_t)boot_u;
  id->seq_u64 = seq_u;
  id->ok = true;
  return true;
}

bool wrap_frame(const char *body, char *dst, uint32_t cap) {
  static const char kPrefix[] = "{\"type\":\"energy_estimate\",\"record\":";
  uint32_t blen = (uint32_t)strlen(body);
  uint32_t need = (uint32_t)sizeof(kPrefix) - 1 + blen + 1;
  if (!dst || need + 1 > cap || need > kUploadFrameMax || blen < 2 || body[0] != '{') return false;
  memcpy(dst, kPrefix, sizeof(kPrefix) - 1);
  memcpy(dst + sizeof(kPrefix) - 1, body, blen);
  dst[sizeof(kPrefix) - 1 + blen] = '}';
  dst[sizeof(kPrefix) + blen] = 0;
  return true;
}

bool same_identity(const Fields &f, const char *device, const char *epoch, const char *boot, const char *seq, uint32_t clock,
                   bool simulation) {
  return f.device_set && f.epoch_set && f.boot_set && f.seq_set && f.clock_set && f.simulation_set &&
         strcmp(f.device_id, device) == 0 && strcmp(f.meter_epoch, epoch) == 0 && strcmp(f.boot_id, boot) == 0 &&
         strcmp(f.seq, seq) == 0 && f.clock_version == clock && f.simulation == simulation;
}

bool negative_for_inflight(const Fields &f, const char *device, const char *epoch, const char *boot, const char *seq,
                           uint32_t clock) {
  if (f.device_set && strcmp(f.device_id, device) != 0) return false;
  if (f.epoch_set && strcmp(f.meter_epoch, epoch) != 0) return false;
  if (f.boot_set && strcmp(f.boot_id, boot) != 0) return false;
  if (f.seq_set && strcmp(f.seq, seq) != 0) return false;
  if (f.clock_set && f.clock_version != clock) return false;
  return true;
}

}  // namespace

UploadSession::UploadSession()
    : phase_(Phase::Idle), link_(false), ready_at_(0), sent_ms_(0), backoff_ms_(kUploadRetryMinMs), last_record_id_(0),
      boot_u32_(0), seq_u64_(0), clock_version_(0), simulation_(false) {
  diagnostic_[0] = 0;
  memcpy(diagnostic_, "reset", 6);
  body_[0] = 0;
  device_id_[0] = 0;
  meter_epoch_[0] = 0;
  boot_id_[0] = 0;
  seq_[0] = 0;
}

void UploadSession::set_diag(const char *token) {
  if (!token || !token[0]) token = "none";
  uint32_t i = 0;
  for (; token[i] && i + 1 < sizeof(diagnostic_); ++i) {
    char c = token[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    diagnostic_[i] = ok ? c : '_';
  }
  diagnostic_[i] = 0;
}

void UploadSession::reset(uint32_t now_ms) {
  phase_ = Phase::Idle;
  link_ = false;
  ready_at_ = now_ms;
  sent_ms_ = now_ms;
  backoff_ms_ = kUploadRetryMinMs;
  body_[0] = 0;
  set_diag("reset");
}

void UploadSession::on_link(bool up, uint32_t now_ms) {
  link_ = up;
  if (!up) {
    if (phase_ == Phase::Inflight || phase_ == Phase::Armed) set_diag("disconnected");
    phase_ = Phase::Idle;
    ready_at_ = now_ms;
  }
}

bool UploadSession::ready(uint32_t now_ms) const { return (int32_t)(now_ms - ready_at_) >= 0; }

void UploadSession::backoff(uint32_t now_ms) {
  ready_at_ = now_ms + backoff_ms_;
  if (backoff_ms_ < kUploadRetryMaxMs / 2) backoff_ms_ *= 2;
  else backoff_ms_ = kUploadRetryMaxMs;
  phase_ = Phase::Idle;
}

bool UploadSession::arm(Meter &meter, uint32_t now_ms, char *dst, uint32_t cap) {
  if (phase_ == Phase::Inflight && (uint32_t)(now_ms - sent_ms_) >= kAckWaitMs) {
    set_diag("ack_timeout");
    backoff(now_ms);
  }
  if (!link_ || phase_ != Phase::Idle || !ready(now_ms) || !dst || cap < 2) return false;
  static char scratch[kDefaultSlotBytes];
  bool held = false;
  for (uint32_t ordinal = 0; ordinal < 64; ++ordinal) {
    if (!meter.record_json(ordinal, scratch, sizeof(scratch))) break;
    Identity id;
    if (!parse_identity(scratch, &id) || !wrap_frame(scratch, dst, cap)) {
      held = true;
      continue;
    }
    memcpy(body_, scratch, sizeof(body_));
    memcpy(device_id_, id.device_id, sizeof(device_id_));
    memcpy(meter_epoch_, id.meter_epoch, sizeof(meter_epoch_));
    memcpy(boot_id_, id.boot_id, sizeof(boot_id_));
    memcpy(seq_, id.seq, sizeof(seq_));
    boot_u32_ = id.boot_u32;
    seq_u64_ = id.seq_u64;
    clock_version_ = id.clock_version;
    simulation_ = id.simulation;
    phase_ = Phase::Armed;
    if (held) set_diag("journal_body_held");
    return true;
  }
  if (held) set_diag("journal_body_held");
  return false;
}

void UploadSession::sent(uint32_t now_ms) {
  if (phase_ != Phase::Armed) return;
  phase_ = Phase::Inflight;
  sent_ms_ = now_ms;
}

void UploadSession::send_failed(uint32_t now_ms) {
  if (phase_ != Phase::Armed && phase_ != Phase::Inflight) return;
  set_diag("send_failed");
  backoff(now_ms);
}

void UploadSession::on_text(Meter &meter, const char *json, uint32_t now_ms) {
  if (phase_ != Phase::Inflight || !json) return;
  Fields fields;
  if (!parse_fields(json, &fields)) {
    set_diag("malformed");
    return;
  }
  FrameKind kind = classify(fields);
  if (kind == FrameKind::Unrelated || kind == FrameKind::Malformed) {
    if (kind == FrameKind::Malformed) set_diag("malformed");
    return;
  }
  if (kind == FrameKind::Durable) {
    if (!same_identity(fields, device_id_, meter_epoch_, boot_id_, seq_, clock_version_, simulation_)) {
      set_diag("identity_mismatch");
      backoff(now_ms);
      return;
    }
    char saved[kDefaultSlotBytes];
    memcpy(saved, body_, sizeof(saved));
    bool still = false;
    static char probe[kDefaultSlotBytes];
    for (uint32_t i = 0; meter.record_json(i, probe, sizeof(probe)); ++i) {
      if (strcmp(probe, saved) == 0) {
        still = true;
        break;
      }
    }
    if (!still || !meter.note_server_commit_replay(boot_u32_, seq_u64_, clock_version_, saved)) {
      set_diag("retire_not_durable");
      backoff(now_ms);
      memcpy(body_, saved, sizeof(body_));
      return;
    }
    char again[kDefaultSlotBytes];
    for (uint32_t i = 0; meter.record_json(i, again, sizeof(again)); ++i) {
      if (strcmp(again, saved) == 0) {
        set_diag("retire_not_durable");
        backoff(now_ms);
        return;
      }
    }
    last_record_id_ = fields.record_id;
    backoff_ms_ = kUploadRetryMinMs;
    ready_at_ = now_ms;
    phase_ = Phase::Idle;
    set_diag("retired");
    return;
  }
  if (kind == FrameKind::Negative) {
    if (!negative_for_inflight(fields, device_id_, meter_epoch_, boot_id_, seq_, clock_version_)) {
      set_diag("identity_mismatch");
      return;
    }
    set_diag(fields.error);
    backoff(now_ms);
    return;
  }
  set_diag("not_durable");
  backoff(now_ms);
}

}  // namespace loadac
