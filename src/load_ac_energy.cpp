#include "load_ac_energy.h"

#include <stdio.h>
#include <string.h>

namespace loadac {
namespace {

constexpr uint32_t kMagic = 0x31454341u;
constexpr uint16_t kVersion = 1;

uint32_t crc32_ieee(const uint8_t *data, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; ++i) {
    c ^= data[i];
    for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get_u64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

struct Buf {
  char *p;
  uint32_t n;
  uint32_t cap;
  bool ok;
};

void add(Buf *b, const char *s) {
  if (!b->ok || !s) return;
  while (*s) {
    if (b->n + 1 >= b->cap) { b->ok = false; return; }
    b->p[b->n++] = *s++;
  }
  b->p[b->n] = 0;
}

void add_u64(Buf *b, uint64_t v) {
  char tmp[24];
  char rev[24];
  int n = 0;
  if (v == 0) rev[n++] = '0';
  while (v) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
  int k = 0;
  while (n) tmp[k++] = rev[--n];
  tmp[k] = 0;
  add(b, tmp);
}

void add_u32(Buf *b, uint32_t v) { add_u64(b, v); }

void safe_copy(char *dst, size_t n, const char *src) {
  if (n == 0) return;
  if (!src) src = "unknown";
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < n; ++i) {
    char c = src[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == ':' || c == '-';
    dst[j++] = ok ? c : '_';
  }
  dst[j] = 0;
}

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((a < 0) != (b < 0))) --q;
  return q;
}

void civil_from_days(int64_t z, int *y, int *m, int *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int year = (int)yoe + (int)era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  year += (month <= 2);
  *y = year;
  *m = (int)month;
  *d = (int)day;
}

int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int bcd_byte(uint8_t v, int *out) {
  int hi = (v >> 4) & 0x0F;
  int lo = v & 0x0F;
  if (hi > 9 || lo > 9) return -1;
  *out = hi * 10 + lo;
  return 0;
}

uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

struct Header {
  uint64_t epoch;
  uint64_t seq;
  uint32_t boot;
  uint32_t flags;
  uint64_t energy;
  uint64_t residue;
  int32_t pf;
  uint32_t model;
  uint32_t commits;
  uint32_t loss;
  uint32_t reason;
  uint32_t adjustments;
  uint32_t commits_in_window;
  uint64_t allocated;
  uint64_t unallocated;
  uint64_t window_start;
  uint8_t tail_open;
  uint8_t counter_trusted;
  uint8_t journal_untrusted;
  uint8_t source_blocked;
  int16_t day_y;
  uint8_t day_m;
  uint8_t day_d;
  uint8_t day_measured;
  uint16_t slot_count;
  uint16_t slot_bytes;
  uint32_t source_version;
  uint32_t header_writes;
  uint32_t day_gaps;
  uint64_t day_covered;
  uint64_t day_mono_start;
  char device_id[16];
  bool valid;
};

void blank_header(Header *h, const ModelConfig &cfg) {
  memset(h, 0, sizeof(*h));
  h->epoch = 1;
  h->seq = 1;
  h->boot = 1;
  h->pf = cfg.pf_assumed_millis;
  h->model = cfg.model_revision;
  h->source_version = cfg.source_version;
  h->slot_count = cfg.slot_count;
  h->slot_bytes = cfg.slot_bytes;
  h->counter_trusted = 1;
  memcpy(h->device_id, "unknown", 8);
  h->valid = true;
}

void encode_header(const Header &h, uint8_t raw[kHeaderBytes]) {
  memset(raw, 0, kHeaderBytes);
  put_u32(raw + 0, kMagic);
  put_u16(raw + 4, kVersion);
  put_u16(raw + 6, kHeaderBytes);
  put_u64(raw + 8, h.epoch);
  put_u64(raw + 16, h.seq);
  put_u32(raw + 24, h.boot);
  put_u32(raw + 28, h.flags);
  put_u64(raw + 32, h.energy);
  put_u64(raw + 40, h.residue);
  put_u32(raw + 48, (uint32_t)h.pf);
  put_u32(raw + 52, h.model);
  put_u32(raw + 56, h.commits);
  put_u32(raw + 60, h.loss);
  put_u32(raw + 64, h.reason);
  put_u32(raw + 68, h.adjustments);
  put_u32(raw + 72, h.commits_in_window);
  put_u64(raw + 80, h.allocated);
  put_u64(raw + 88, h.unallocated);
  put_u64(raw + 96, h.window_start);
  raw[104] = h.tail_open;
  raw[105] = h.counter_trusted;
  raw[106] = h.journal_untrusted;
  raw[107] = h.source_blocked;
  put_u16(raw + 108, (uint16_t)h.day_y);
  raw[110] = h.day_m;
  raw[111] = h.day_d;
  raw[112] = h.day_measured;
  put_u16(raw + 114, h.slot_count);
  put_u16(raw + 116, h.slot_bytes);
  put_u32(raw + 120, h.source_version);
  put_u32(raw + 124, h.header_writes);
  put_u32(raw + 128, h.day_gaps);
  put_u64(raw + 136, h.day_covered);
  put_u64(raw + 144, h.day_mono_start);
  memcpy(raw + 160, h.device_id, 16);
  put_u32(raw + 248, crc32_ieee(raw, 248));
}

bool decode_header(const uint8_t raw[kHeaderBytes], Header *h) {
  memset(h, 0, sizeof(*h));
  if (get_u32(raw + 0) != kMagic || get_u16(raw + 4) != kVersion) return false;
  if (get_u32(raw + 248) != crc32_ieee(raw, 248)) return false;
  h->epoch = get_u64(raw + 8);
  h->seq = get_u64(raw + 16);
  h->boot = get_u32(raw + 24);
  h->flags = get_u32(raw + 28);
  h->energy = get_u64(raw + 32);
  h->residue = get_u64(raw + 40);
  h->pf = (int32_t)get_u32(raw + 48);
  h->model = get_u32(raw + 52);
  h->commits = get_u32(raw + 56);
  h->loss = get_u32(raw + 60);
  h->reason = get_u32(raw + 64);
  h->adjustments = get_u32(raw + 68);
  h->commits_in_window = get_u32(raw + 72);
  h->allocated = get_u64(raw + 80);
  h->unallocated = get_u64(raw + 88);
  h->window_start = get_u64(raw + 96);
  h->tail_open = raw[104];
  h->counter_trusted = raw[105];
  h->journal_untrusted = raw[106];
  h->source_blocked = raw[107];
  h->day_y = (int16_t)get_u16(raw + 108);
  h->day_m = raw[110];
  h->day_d = raw[111];
  h->day_measured = raw[112];
  h->slot_count = get_u16(raw + 114);
  h->slot_bytes = get_u16(raw + 116);
  h->source_version = get_u32(raw + 120);
  h->header_writes = get_u32(raw + 124);
  h->day_gaps = get_u32(raw + 128);
  h->day_covered = get_u64(raw + 136);
  h->day_mono_start = get_u64(raw + 144);
  memcpy(h->device_id, raw + 160, 16);
  h->device_id[15] = 0;
  h->valid = true;
  return true;
}

bool region_blank(const uint8_t *p, uint32_t n) {
  bool zeros = true, ones = true;
  for (uint32_t i = 0; i < n; ++i) {
    if (p[i] != 0x00) zeros = false;
    if (p[i] != 0xFF) ones = false;
  }
  return zeros || ones;
}

}  // namespace

uint32_t crc32_ieee_bytes(const uint8_t *p, uint32_t n) { return crc32_ieee(p, n); }

ModelConfig default_model() {
  ModelConfig c;
  c.model_revision = kModelRevision;
  c.source_version = 1;
  c.pf_assumed_millis = kPfAssumedMillis;
  c.max_integrate_ms = 2000;
  c.commit_interval_ms = 300000;
  c.wear_window_ms = 86400000ul;
  c.max_commits_per_window = 400;
  c.slot_count = (uint16_t)kDefaultSlots;
  c.slot_bytes = (uint16_t)kDefaultSlotBytes;
  return c;
}

const char *reason_name(UnavailableReason r) {
  switch (r) {
    case UnavailableReason::None: return "none";
    case UnavailableReason::SourceUnavailable: return "source_unavailable";
    case UnavailableReason::Stm32TextVoutIoutNotRms: return "stm32_text_vout_iout_not_rms";
    case UnavailableReason::MissForcedZero: return "miss_forced_zero";
    case UnavailableReason::RmsValidityFalse: return "rms_validity_false";
    case UnavailableReason::CoverageUnknown: return "coverage_unknown";
    case UnavailableReason::ModeUnknown: return "mode_unknown";
    case UnavailableReason::BypassNotCovered: return "bypass_not_covered";
    case UnavailableReason::BmsDcDischarge: return "bms_dc_discharge";
    case UnavailableReason::AdsDcPvBus: return "ads1115_dc_pv_bus";
    case UnavailableReason::PfNotAssumption: return "pf_not_flagged_assumption";
    case UnavailableReason::SampleMissing: return "sample_missing";
    case UnavailableReason::SampleInvalid: return "sample_invalid";
  }
  return "source_unavailable";
}

const char *mode_name(Mode m) {
  switch (m) {
    case Mode::Idle: return "idle";
    case Mode::Inverter: return "inverter";
    case Mode::GridBypass: return "grid_bypass";
    case Mode::Transfer: return "transfer";
    case Mode::Fault: return "fault";
    case Mode::Unknown: break;
  }
  return "unknown";
}

const char *coverage_name(Coverage c) {
  switch (c) {
    case Coverage::InverterOutputOnly: return "inverter_output_only";
    case Coverage::CommonOutput: return "common_output";
    case Coverage::Unknown: break;
  }
  return "unknown";
}

const char *quality_name(TimeQuality q) {
  switch (q) {
    case TimeQuality::Unanchored: return "unanchored";
    case TimeQuality::PlausibleTick: return "plausible_tick";
    case TimeQuality::Anchored: return "anchored";
    case TimeQuality::Adjusted: return "adjusted";
    case TimeQuality::StaleNvsRejected: return "stale_nvs_rejected";
    case TimeQuality::Uncertain: break;
  }
  return "uncertain";
}

const char *attribution_name(Attribution a) {
  switch (a) {
    case Attribution::Allocated: return "allocated";
    case Attribution::ValidZero: return "valid_zero";
    case Attribution::Unallocated: break;
  }
  return "unallocated";
}

const char *kind_name(RecordKind k) {
  switch (k) {
    case RecordKind::Interval: return "interval";
    case RecordKind::Gap: return "gap";
    case RecordKind::DailyCheckpoint: return "daily_checkpoint";
    case RecordKind::Status: break;
  }
  return "status";
}

bool coverage_allows(Mode mode, Coverage coverage) {
  if (coverage == Coverage::Unknown || mode == Mode::Unknown || mode == Mode::Idle ||
      mode == Mode::Transfer || mode == Mode::Fault)
    return false;
  if (coverage == Coverage::CommonOutput) return mode == Mode::Inverter || mode == Mode::GridBypass;
  if (coverage == Coverage::InverterOutputOnly) return mode == Mode::Inverter;
  return false;
}

bool is_leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

bool civil_valid(int year, int month, int day) {
  if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1) return false;
  static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int dim = mdays[month - 1];
  if (month == 2 && is_leap_year(year)) dim = 29;
  return day <= dim;
}

bool lagos_day(int64_t unix_utc, CivilDay *out) {
  if (!out) return false;
  out->valid = false;
  int64_t local = unix_utc + kLagosOffsetSeconds;
  int64_t days = floor_div(local, 86400);
  int y, m, d;
  civil_from_days(days, &y, &m, &d);
  if (!civil_valid(y, m, d) && (y < 1970 || y > 2099)) return false;
  out->year = y;
  out->month = m;
  out->day = d;
  out->valid = civil_valid(y, m, d);
  return out->valid;
}

bool same_lagos_day(int64_t a, int64_t b) {
  CivilDay da, db;
  if (!lagos_day(a, &da) || !lagos_day(b, &db)) return false;
  return da.year == db.year && da.month == db.month && da.day == db.day;
}

int64_t unix_from_civil_utc(int year, int month, int day, int hour, int minute, int second) {
  if (!civil_valid(year, month, day) || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 59)
    return 0;
  return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

bool mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
  if (!out) return false;
  if (a != 0 && b > UINT64_MAX / a) return false;
  *out = a * b;
  return true;
}

bool estimated_mwh(int32_t v_mV, int32_t i_mA, int32_t pf_millis, uint64_t dt_ms, uint64_t *residue,
                   uint64_t *added_mwh) {
  if (!residue || !added_mwh || v_mV < 0 || i_mA < 0 || pf_millis <= 0 || pf_millis > 1000) return false;
  uint64_t num = 0;
  if (!mul_u64((uint64_t)v_mV, (uint64_t)i_mA, &num) || !mul_u64(num, (uint64_t)pf_millis, &num) ||
      !mul_u64(num, dt_ms, &num))
    return false;
  if (*residue > UINT64_MAX - num) return false;
  num += *residue;
  *added_mwh = num / kEnergyDenom;
  *residue = num % kEnergyDenom;
  return true;
}

static int decode_clock_face(const uint8_t reg[7], int *y, int *mo, int *d, int *h, int *mi, int *s, bool *halted) {
  *halted = (reg[0] & 0x80) != 0;
  if (bcd_byte((uint8_t)(reg[0] & 0x7F), s) || *s > 59) return -1;
  if ((reg[1] & 0x80) || bcd_byte((uint8_t)(reg[1] & 0x7F), mi) || *mi > 59) return -1;
  if (reg[2] & 0x40) {
    int hr12 = 0;
    if (bcd_byte((uint8_t)(reg[2] & 0x1F), &hr12) || hr12 < 1 || hr12 > 12) return -1;
    bool pm = (reg[2] & 0x20) != 0;
    *h = (hr12 % 12) + (pm ? 12 : 0);
  } else {
    if ((reg[2] & 0x80) || bcd_byte((uint8_t)(reg[2] & 0x3F), h) || *h > 23) return -1;
  }
  int dow = 0;
  if (bcd_byte((uint8_t)(reg[3] & 0x07), &dow) || dow < 1 || dow > 7) return -1;
  if (bcd_byte((uint8_t)(reg[4] & 0x3F), d)) return -1;
  if (bcd_byte((uint8_t)(reg[5] & 0x1F), mo)) return -1;
  int yy = 0;
  if (bcd_byte(reg[6], &yy)) return -1;
  *y = 2000 + yy;
  if (!civil_valid(*y, *mo, *d)) return -2;
  return 0;
}

RtcReading decode_ds1307(const uint8_t reg[7]) {
  RtcReading r;
  memset(&r, 0, sizeof(r));
  int y, mo, d, h, mi, s;
  bool halted = false;
  int rc = decode_clock_face(reg, &y, &mo, &d, &h, &mi, &s, &halted);
  r.halted = halted;
  if (rc != 0) return r;
  r.face.year = y;
  r.face.month = mo;
  r.face.day = d;
  r.face.valid = true;
  r.unix_utc = unix_from_civil_utc(y, mo, d, h, mi, s);
  r.ok = !halted;
  return r;
}

bool ds1307_pair(const uint8_t first[7], const uint8_t second[7], RtcReading *out) {
  if (!out) return false;
  RtcReading a = decode_ds1307(first);
  RtcReading b = decode_ds1307(second);
  if (a.halted || b.halted) {
    *out = b.halted ? b : a;
    out->ok = false;
    out->halted = true;
    return false;
  }
  if (!a.ok || !b.ok) return false;
  int64_t delta = b.unix_utc - a.unix_utc;
  if (delta < 0) delta = -delta;
  if (delta > 2) return false;
  *out = b.unix_utc >= a.unix_utc ? b : a;
  return true;
}

void encode_ds1307(int year, int month, int day, int hour, int minute, int second, uint8_t reg[7]) {
  memset(reg, 0, 7);
  if (!civil_valid(year, month, day) || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 59 || year < 2000 || year > 2099)
    return;
  reg[0] = to_bcd(second);
  reg[1] = to_bcd(minute);
  reg[2] = to_bcd(hour);
  reg[3] = 1;
  reg[4] = to_bcd(day);
  reg[5] = to_bcd(month);
  reg[6] = to_bcd(year - 2000);
}

bool ds1307_has_osf_bit() { return false; }

bool i2c_allow(const I2cBackoff &b, uint32_t now_ms) { return now_ms >= b.suppress_until_ms; }

void i2c_fail(I2cBackoff *b, uint32_t now_ms) {
  if (!b) return;
  if (b->backoff_ms < 100) b->backoff_ms = 100;
  else if (b->backoff_ms < 60000) b->backoff_ms *= 2;
  if (b->backoff_ms > 60000) b->backoff_ms = 60000;
  b->suppress_until_ms = now_ms + b->backoff_ms;
  b->failures++;
}

void i2c_ok(I2cBackoff *b) {
  if (!b) return;
  b->backoff_ms = 100;
  b->suppress_until_ms = 0;
  b->failures = 0;
}

bool json_has_ambiguous_energy_key(const char *json) {
  static const char *bad[] = {"energy", "wh", "kwh", "mwh", "energy_wh", "energy_kwh", "inverter_energy",
                              "consumed_wh", "energy_consumed", "output_energy", "output_energy_wh", 0};
  if (!json) return false;
  for (const char *p = json; *p; ++p) {
    if (*p != '"') continue;
    const char *start = ++p;
    while (*p && *p != '"') ++p;
    if (*p != '"') return false;
    size_t n = (size_t)(p - start);
    if (p[1] != ':') continue;
    for (int i = 0; bad[i]; ++i) {
      if (strlen(bad[i]) == n && memcmp(start, bad[i], n) == 0) return true;
    }
  }
  return false;
}

AcSample qualify_stm32(const Stm32Observation &in, UnavailableReason *why) {
  AcSample s;
  memset(&s, 0, sizeof(s));
  s.mono_ms = in.mono_ms;
  s.mode = in.has_mode ? in.mode : Mode::Unknown;
  s.coverage = in.has_coverage ? in.coverage : Coverage::Unknown;
  s.source_version = in.source_version ? in.source_version : 1;
  s.source_path = in.source_path ? in.source_path : "stm32_uart1_rx33_tx2_legacy_text";
  UnavailableReason reason = UnavailableReason::Stm32TextVoutIoutNotRms;
  if (in.miss_forced_zero) reason = UnavailableReason::MissForcedZero;
  else if (in.has_vrms && in.has_irms) {
    if (!in.vrms_valid || !in.irms_valid || in.vrms_mV < 0 || in.irms_mA < 0)
      reason = UnavailableReason::RmsValidityFalse;
    else if (!in.has_coverage || in.coverage == Coverage::Unknown)
      reason = UnavailableReason::CoverageUnknown;
    else if (!in.has_mode || in.mode == Mode::Unknown)
      reason = UnavailableReason::ModeUnknown;
    else {
      s.acquired = true;
      s.valid = true;
      s.v_rms_mV = in.vrms_mV;
      s.i_rms_mA = in.irms_mA;
      reason = coverage_allows(s.mode, s.coverage) ? UnavailableReason::None : UnavailableReason::BypassNotCovered;
    }
  }
  if (why) *why = reason;
  return s;
}

AcSample reject_bms_dc(uint64_t mono_ms) {
  AcSample s;
  memset(&s, 0, sizeof(s));
  s.mono_ms = mono_ms;
  s.source_path = "jk_bms_dc_discharge";
  return s;
}

AcSample reject_ads_pv(uint64_t mono_ms) {
  AcSample s;
  memset(&s, 0, sizeof(s));
  s.mono_ms = mono_ms;
  s.source_path = "ads1115_dc_pv_bus";
  return s;
}

Stm32Observation pinned_main_stm32_observation(uint64_t mono_ms) {
  Stm32Observation o;
  memset(&o, 0, sizeof(o));
  o.has_vout = true;
  o.has_iout = true;
  o.mono_ms = mono_ms;
  o.source_version = 1;
  o.source_path = "stm32_uart1_rx33_tx2_legacy_text";
  return o;
}

MemoryStore::MemoryStore() : fail_persists_(0) {
  memset(live_, 0, sizeof(live_));
  memset(durable_, 0, sizeof(durable_));
}

bool MemoryStore::read(uint32_t offset, uint8_t *dst, uint32_t n) {
  if (!dst || offset + n > kStoreBytes) return false;
  memcpy(dst, live_ + offset, n);
  return true;
}

bool MemoryStore::write(uint32_t offset, const uint8_t *src, uint32_t n) {
  if (!src || offset + n > kStoreBytes) return false;
  memcpy(live_ + offset, src, n);
  return true;
}

bool MemoryStore::persist() {
  if (fail_persists_ > 0) {
    --fail_persists_;
    memcpy(live_, durable_, sizeof(live_));
    return false;
  }
  memcpy(durable_, live_, sizeof(live_));
  return true;
}

void MemoryStore::clone_durable(MemoryStore &dst) const {
  memcpy(dst.live_, durable_, sizeof(durable_));
  memcpy(dst.durable_, durable_, sizeof(durable_));
  dst.fail_persists_ = 0;
}

bool MemoryStore::durable_equals(const MemoryStore &o) const {
  return memcmp(durable_, o.durable_, sizeof(durable_)) == 0;
}

namespace {

UnavailableReason uncover_reason(const AcSample &s) {
  if (!s.acquired || !s.valid) return UnavailableReason::SampleInvalid;
  if (s.mode == Mode::Unknown) return UnavailableReason::ModeUnknown;
  if (s.coverage == Coverage::Unknown) return UnavailableReason::CoverageUnknown;
  if (s.coverage == Coverage::InverterOutputOnly &&
      (s.mode == Mode::GridBypass || s.mode == Mode::Transfer))
    return UnavailableReason::BypassNotCovered;
  return UnavailableReason::SampleMissing;
}

}  // namespace

Meter::Meter()
    : store_(0), ready_(false), counter_trusted_(false), journal_untrusted_(false), overflow_(false),
      source_blocked_(true), source_qualified_(false), tail_open_(false),
      blocked_reason_(UnavailableReason::Stm32TextVoutIoutNotRms), epoch_(1), seq_(1), boot_(0), energy_(0),
      residue_(0), persisted_energy_(0), commits_(0), loss_(0), allocated_(0), unallocated_(0), window_start_(0),
      commits_in_window_(0), have_last_(false), last_pf_(kPfAssumedMillis), day_covered_ms_(0),
      day_mono_start_(0), day_gaps_(0), day_measured_(false), header_writes_(0), durable_energy_(0),
      durable_residue_(0) {
  memset(&cfg_, 0, sizeof(cfg_));
  memset(device_id_, 0, sizeof(device_id_));
  memcpy(device_id_, "unknown", 8);
  memset(&seg_, 0, sizeof(seg_));
  memset(&clock_, 0, sizeof(clock_));
  clock_.quality = TimeQuality::Uncertain;
  memset(&last_, 0, sizeof(last_));
  memset(&day_, 0, sizeof(day_));
}

bool Meter::save_header(bool persist_now) {
  Header h;
  memset(&h, 0, sizeof(h));
  h.epoch = epoch_;
  h.seq = seq_;
  h.boot = boot_;
  h.flags = (overflow_ ? 1u : 0u) | (journal_untrusted_ ? 2u : 0u);
  h.energy = durable_energy_;
  h.residue = durable_residue_;
  h.pf = cfg_.pf_assumed_millis;
  h.model = cfg_.model_revision;
  h.commits = commits_;
  h.loss = loss_;
  h.reason = (uint32_t)blocked_reason_;
  h.adjustments = clock_.adjustments;
  h.commits_in_window = commits_in_window_;
  h.allocated = allocated_;
  h.unallocated = unallocated_;
  h.window_start = window_start_;
  h.tail_open = tail_open_ ? 1 : 0;
  h.counter_trusted = counter_trusted_ ? 1 : 0;
  h.journal_untrusted = journal_untrusted_ ? 1 : 0;
  h.source_blocked = source_blocked_ ? 1 : 0;
  h.day_y = day_.valid ? (int16_t)day_.year : 0;
  h.day_m = day_.valid ? (uint8_t)day_.month : 0;
  h.day_d = day_.valid ? (uint8_t)day_.day : 0;
  h.day_measured = day_measured_ ? 1 : 0;
  h.slot_count = cfg_.slot_count;
  h.slot_bytes = cfg_.slot_bytes;
  h.source_version = cfg_.source_version;
  h.header_writes = ++header_writes_;
  h.day_gaps = day_gaps_;
  h.day_covered = day_covered_ms_;
  h.day_mono_start = day_mono_start_;
  memcpy(h.device_id, device_id_, 16);
  uint8_t raw[kHeaderBytes];
  encode_header(h, raw);
  if (!store_->write(0, raw, kHeaderBytes)) return false;
  if (persist_now && !store_->persist()) return false;
  return true;
}

bool Meter::persist_image() { return store_ && store_->persist(); }

bool Meter::begin(ByteStore &store, const ModelConfig &cfg) {
  store_ = &store;
  cfg_ = cfg;
  ready_ = false;
  if (cfg_.pf_assumed_millis <= 0 || cfg_.pf_assumed_millis > 1000) return false;
  if (cfg_.slot_count == 0 || cfg_.slot_bytes < 320) return false;
  if ((uint32_t)cfg_.slot_count * cfg_.slot_bytes + kHeaderBytes > store.size()) return false;
  uint8_t raw[kHeaderBytes];
  if (!store_->read(0, raw, kHeaderBytes)) return false;
  Header h;
  bool blank = region_blank(raw, kHeaderBytes);
  bool decoded = !blank && decode_header(raw, &h);
  if (!decoded) {
    blank_header(&h, cfg_);
    if (!blank) {
      h.journal_untrusted = 1;
      h.loss = 1;
      h.counter_trusted = 1;
    }
  } else {
    h.boot += 1;
    if (h.tail_open) {
      h.loss += 1;
      h.tail_open = 0;
    }
    h.seq = 1;
    h.energy = 0;
    h.residue = 0;
  }
  if (!h.model) h.model = cfg_.model_revision;
  epoch_ = h.epoch ? h.epoch : 1;
  seq_ = h.seq ? h.seq : 1;
  boot_ = h.boot ? h.boot : 1;
  durable_energy_ = decoded ? h.energy : 0;
  durable_residue_ = decoded ? h.residue : 0;
  energy_ = durable_energy_;
  residue_ = durable_residue_;
  persisted_energy_ = durable_energy_;
  commits_ = h.commits;
  loss_ = h.loss;
  clock_.adjustments = h.adjustments;
  commits_in_window_ = h.commits_in_window;
  allocated_ = decoded ? h.allocated : 0;
  unallocated_ = decoded ? h.unallocated : 0;
  window_start_ = h.window_start;
  tail_open_ = false;
  counter_trusted_ = h.counter_trusted != 0;
  journal_untrusted_ = h.journal_untrusted != 0;
  overflow_ = (h.flags & 1u) != 0;
  source_blocked_ = true;
  source_qualified_ = false;
  blocked_reason_ = UnavailableReason::Stm32TextVoutIoutNotRms;
  cfg_.model_revision = kModelRevision;
  cfg_.pf_assumed_millis = kPfAssumedMillis;
  day_covered_ms_ = decoded ? h.day_covered : 0;
  day_gaps_ = decoded ? h.day_gaps : 0;
  day_measured_ = decoded && h.day_measured;
  day_mono_start_ = decoded ? h.day_mono_start : 0;
  header_writes_ = h.header_writes;
  day_.year = h.day_y;
  day_.month = h.day_m;
  day_.day = h.day_d;
  day_.valid = h.day_m >= 1 && h.day_d >= 1;
  safe_copy(device_id_, sizeof(device_id_), h.device_id[0] ? h.device_id : "unknown");
  memset(&seg_, 0, sizeof(seg_));
  have_last_ = false;
  last_pf_ = cfg_.pf_assumed_millis;
  clock_.anchored = false;
  clock_.quality = TimeQuality::Uncertain;
  clock_.generation = 1;
  if (!save_header(true)) return false;
  ready_ = true;
  return true;
}

bool Meter::set_device_id(const char *id) {
  if (!ready_ || !id || !id[0]) return false;
  safe_copy(device_id_, sizeof(device_id_), id);
  return save_header(true);
}

void Meter::set_source_blocked(UnavailableReason why) {
  source_blocked_ = true;
  source_qualified_ = false;
  blocked_reason_ = why;
}

bool Meter::utc_at(uint64_t mono_ms, int64_t *utc) const {
  if (!utc || !clock_.anchored || mono_ms < clock_.mono_anchor) return false;
  *utc = clock_.utc_anchor + (int64_t)((mono_ms - clock_.mono_anchor) / 1000ull);
  return true;
}

bool Meter::wear_allow(uint64_t mono_ms) {
  if (window_start_ == 0 || mono_ms < window_start_ || mono_ms - window_start_ >= cfg_.wear_window_ms) {
    window_start_ = mono_ms;
    commits_in_window_ = 0;
  }
  return commits_in_window_ < cfg_.max_commits_per_window;
}

void Meter::note_wear(uint64_t mono_ms) {
  wear_allow(mono_ms);
  commits_in_window_++;
}

bool Meter::write_slot(uint64_t seq, uint32_t boot, const char *json) {
  if (!json || json_has_ambiguous_energy_key(json)) return false;
  uint32_t len = (uint32_t)strlen(json);
  if (len == 0 || 20u + len > cfg_.slot_bytes) return false;
  int free_index = -1;
  for (uint16_t i = 0; i < cfg_.slot_count; ++i) {
    uint8_t state = 0;
    uint32_t off = kHeaderBytes + (uint32_t)i * cfg_.slot_bytes;
    if (!store_->read(off, &state, 1)) return false;
    if (state != 1) { free_index = (int)i; break; }
  }
  if (free_index < 0) {
    overflow_ = true;
    return false;
  }
  uint8_t slot[kDefaultSlotBytes];
  if (cfg_.slot_bytes > kDefaultSlotBytes) return false;
  memset(slot, 0, cfg_.slot_bytes);
  slot[0] = 1;
  slot[2] = (uint8_t)len;
  slot[3] = (uint8_t)(len >> 8);
  put_u64(slot + 8, seq);
  put_u32(slot + 16, boot);
  memcpy(slot + 20, json, len);
  uint32_t crc = crc32_ieee(slot + 8, 12 + len);
  put_u32(slot + 4, crc);
  uint32_t off = kHeaderBytes + (uint32_t)free_index * cfg_.slot_bytes;
  return store_->write(off, slot, cfg_.slot_bytes);
}

static void add_key(Buf *b, const char *key, bool comma) {
  if (comma) add(b, ",");
  add(b, "\"");
  add(b, key);
  add(b, "\":");
}

static void add_str(Buf *b, const char *key, const char *val, bool comma) {
  add_key(b, key, comma);
  add(b, "\"");
  add(b, val ? val : "");
  add(b, "\"");
}

static void add_u64s(Buf *b, const char *key, uint64_t v, bool comma) {
  add_key(b, key, comma);
  add(b, "\"");
  add_u64(b, v);
  add(b, "\"");
}

static void add_null_or_u64(Buf *b, const char *key, bool present, uint64_t v, bool comma) {
  add_key(b, key, comma);
  if (!present) add(b, "null");
  else { add(b, "\""); add_u64(b, v); add(b, "\""); }
}

static void add_bool(Buf *b, const char *key, bool v, bool comma) {
  add_key(b, key, comma);
  add(b, v ? "true" : "false");
}

uint32_t Meter::copy_records(char *dst, uint32_t dst_bytes, uint32_t *count) const {
  if (count) *count = 0;
  if (!dst || dst_bytes == 0) return 0;
  dst[0] = 0;
  uint32_t used = 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < 64; ++i) {
    char one[kDefaultSlotBytes];
    if (!record_json(i, one, sizeof(one))) break;
    uint32_t len = (uint32_t)strlen(one);
    if (used + len + 2 >= dst_bytes) break;
    if (used) dst[used++] = '\n';
    memcpy(dst + used, one, len);
    used += len;
    dst[used] = 0;
    n++;
  }
  if (count) *count = n;
  return used;
}

bool Meter::record_json(uint32_t ordinal, char *dst, uint32_t dst_bytes) const {
  if (!store_ || !dst || dst_bytes < 2) return false;
  uint64_t seqs[64];
  uint16_t index[64];
  uint32_t n = 0;
  for (uint16_t i = 0; i < cfg_.slot_count && n < 64; ++i) {
    uint8_t slot[kDefaultSlotBytes];
    if (cfg_.slot_bytes > sizeof(slot)) return false;
    uint32_t off = kHeaderBytes + (uint32_t)i * cfg_.slot_bytes;
    if (!store_->read(off, slot, cfg_.slot_bytes)) return false;
    if (slot[0] != 1) continue;
    uint32_t len = (uint32_t)slot[2] | ((uint32_t)slot[3] << 8);
    if (20u + len > cfg_.slot_bytes) continue;
    uint32_t expect = crc32_ieee(slot + 8, 12 + len);
    if (expect != get_u32(slot + 4)) continue;
    seqs[n] = get_u64(slot + 8);
    index[n] = i;
    n++;
  }
  for (uint32_t a = 0; a + 1 < n; ++a) {
    for (uint32_t b = a + 1; b < n; ++b) {
      if (seqs[b] < seqs[a]) {
        uint64_t ts = seqs[a]; seqs[a] = seqs[b]; seqs[b] = ts;
        uint16_t ti = index[a]; index[a] = index[b]; index[b] = ti;
      }
    }
  }
  if (ordinal >= n) return false;
  uint8_t slot[kDefaultSlotBytes];
  uint32_t off = kHeaderBytes + (uint32_t)index[ordinal] * cfg_.slot_bytes;
  if (!store_->read(off, slot, cfg_.slot_bytes)) return false;
  uint32_t len = (uint32_t)slot[2] | ((uint32_t)slot[3] << 8);
  if (len + 1 > dst_bytes) return false;
  memcpy(dst, slot + 20, len);
  dst[len] = 0;
  return true;
}

bool Meter::http_success_kept(uint32_t boot_id, uint64_t seq) const {
  for (uint16_t i = 0; i < cfg_.slot_count; ++i) {
    uint8_t slot[kDefaultSlotBytes];
    if (cfg_.slot_bytes > sizeof(slot)) return false;
    uint32_t off = kHeaderBytes + (uint32_t)i * cfg_.slot_bytes;
    if (!store_->read(off, slot, 20)) return false;
    if (slot[0] == 1 && get_u64(slot + 8) == seq && get_u32(slot + 16) == boot_id) return true;
  }
  return false;
}

bool Meter::note_http_success(uint32_t, uint64_t) { return false; }

bool Meter::note_server_commit(uint32_t, uint64_t) { return false; }

bool Meter::note_server_commit_replay(uint32_t boot_id, uint64_t seq, uint32_t clock_correction_version, const char *body) {
  if (!ready_ || !store_ || !body || !body[0]) return false;
  char marker[48];
  snprintf(marker, sizeof(marker), "\"clock_correction_version\":%u", clock_correction_version);
  for (uint16_t i = 0; i < cfg_.slot_count; ++i) {
    uint8_t slot[kDefaultSlotBytes];
    if (cfg_.slot_bytes > sizeof(slot)) return false;
    uint32_t off = kHeaderBytes + (uint32_t)i * cfg_.slot_bytes;
    if (!store_->read(off, slot, cfg_.slot_bytes)) return false;
    if (get_u64(slot + 8) != seq || get_u32(slot + 16) != boot_id) continue;
    uint32_t len = (uint32_t)slot[2] | ((uint32_t)slot[3] << 8);
    if (20u + len > cfg_.slot_bytes || slot[20 + len] != 0) {
      /* payload is not necessarily NUL-terminated inside the slot */
    }
    if (len + 1 > kDefaultSlotBytes) return false;
    char payload[kDefaultSlotBytes];
    memcpy(payload, slot + 20, len);
    payload[len] = 0;
    const char *marked = strstr(payload, marker);
    if (strcmp(payload, body) != 0 || !marked) return false;
    char marker_tail = marked[strlen(marker)];
    if (marker_tail >= '0' && marker_tail <= '9') return false;
    if (slot[0] == 2) return true;
    if (slot[0] != 1) return false;
    slot[0] = 2;
    if (!store_->write(off, slot, cfg_.slot_bytes)) return false;
    return store_->persist();
  }
  return false;
}

void format_utc_z(int64_t unix_utc, char *out, size_t n) {
  int64_t days = floor_div(unix_utc, 86400);
  int64_t sod = unix_utc - days * 86400;
  if (sod < 0) sod += 86400;
  int y = 1970, m = 1, d = 1;
  civil_from_days(days, &y, &m, &d);
  int hh = (int)(sod / 3600);
  int mm = (int)((sod % 3600) / 60);
  int ss = (int)(sod % 60);
  snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, hh, mm, ss);
}

bool Meter::emit(const Segment &seg, int64_t utc_end, bool utc_end_known, CivilDay day) {
  if (!ready_ || !counter_trusted_) return false;
  uint64_t mono_for_wear = seg.mono_end ? seg.mono_end : seg.mono_start;
  if (!wear_allow(mono_for_wear)) {
    overflow_ = true;
    save_header(true);
    return false;
  }
  bool interval_present = seg.kind != RecordKind::Gap && seg.kind != RecordKind::DailyCheckpoint;
  if (seg.kind == RecordKind::Gap) interval_present = false;
  Attribution attr = seg.attribution;
  bool utc_ok = utc_end_known && seg.utc_start_known && clock_.anchored && seg.anchor_gen_start == clock_.generation;
  int64_t utc_start = seg.utc_start;
  if (!utc_ok) {
    utc_end_known = false;
    if (attr == Attribution::Allocated) attr = Attribution::Unallocated;
  }
  if (attr == Attribution::Allocated && day.valid == false) attr = Attribution::Unallocated;

  const bool measurable = seg.kind == RecordKind::Interval && seg.covered_ms > 0 && seg.pf_millis == kPfAssumedMillis &&
                          coverage_allows(seg.mode, seg.coverage);
  uint64_t published = 0;
  if (measurable) {
    uint64_t ignored = 0;
    if (!estimated_mwh(seg.v_mV, seg.i_mA, kPfAssumedMillis, seg.covered_ms, &ignored, &published)) return false;
  }
  const bool inputs_ok = measurable;
  const char *sample_validity = "absent";
  if (!measurable && seg.reason == UnavailableReason::SampleInvalid) sample_validity = "invalid";
  else if (measurable && seg.covered_ms < (seg.mono_end - seg.mono_start)) sample_validity = "partial";
  else if (measurable) sample_validity = "valid";
  uint32_t interval_ms = seg.mono_end >= seg.mono_start ? (uint32_t)(seg.mono_end - seg.mono_start) : 0;
  if (!measurable) published = 0;

  char json[1200];
  Buf b = {json, 0, sizeof(json), true};
  char boot_text[16];
  snprintf(boot_text, sizeof(boot_text), "%u", boot_);
  char utc_a[32], utc_b[32];
  const char *tq = "absent";
  if (clock_.anchored && utc_ok) tq = "rtc_ds1307";
  else if (clock_.have_face || clock_.quality == TimeQuality::StaleNvsRejected || clock_.quality == TimeQuality::Adjusted ||
           clock_.quality == TimeQuality::PlausibleTick || clock_.quality == TimeQuality::Unanchored)
    tq = "uncertain";
  const char *scope = inputs_ok ? coverage_name(seg.coverage) : "unknown";
  add(&b, "{");
  add_str(&b, "contract", kContract, false);
  add_bool(&b, "simulation", false, true);
  add_str(&b, "device_id", device_id_, true);
  add_str(&b, "stream", kStream, true);
  add_u64s(&b, "meter_epoch", epoch_, true);
  add_str(&b, "boot_id", boot_text, true);
  add_u64s(&b, "seq", seq_, true);
  add_key(&b, "clock_correction_version", true); add_u32(&b, 0);
  add(&b, ",\"model\":{");
  add_str(&b, "revision", kContract, false);
  add_str(&b, "source_path", kModelSourcePath, true);
  add_str(&b, "version", kModelWireVersion, true);
  add_str(&b, "pf_kind", "assumed", true);
  add_key(&b, "pf_assumption_millis", true); add_u32(&b, (uint32_t)kPfAssumedMillis);
  add_str(&b, "coverage_scope", scope, true);
  add(&b, "}");
  add_u64s(&b, "monotonic_start_ms", seg.mono_start, true);
  add_u64s(&b, "monotonic_end_ms", seg.mono_end, true);
  add_key(&b, "utc_start", true);
  if (utc_ok && tq[0] == 'r') { format_utc_z(utc_start, utc_a, sizeof(utc_a)); add(&b, "\""); add(&b, utc_a); add(&b, "\""); }
  else add(&b, "null");
  add_key(&b, "utc_end", true);
  if (utc_ok && utc_end_known && tq[0] == 'r') { format_utc_z(utc_end, utc_b, sizeof(utc_b)); add(&b, "\""); add(&b, utc_b); add(&b, "\""); }
  else add(&b, "null");
  uint64_t counter_end = durable_energy_ + published;
  add_str(&b, "time_quality", tq, true);
  add_u64s(&b, "counter_start_mwh", seg.counter_start, true);
  add_u64s(&b, "counter_end_mwh", counter_end, true);
  add_u64s(&b, "interval_estimated_mwh", published, true);
  add_u64s(&b, "covered_ms", inputs_ok ? seg.covered_ms : 0, true);
  add_u64s(&b, "interval_ms", interval_ms, true);
  add_str(&b, "sample_validity", sample_validity, true);
  add(&b, ",\"source_mode_coverage\":{");
  add_u64s(&b, "inverter_output_only_ms", (inputs_ok && seg.coverage == Coverage::InverterOutputOnly) ? seg.covered_ms : 0, false);
  add_u64s(&b, "common_output_ms", (inputs_ok && seg.coverage == Coverage::CommonOutput) ? seg.covered_ms : 0, true);
  add_u64s(&b, "unknown_ms", inputs_ok ? 0 : interval_ms, true);
  add(&b, "}");
  if (!inputs_ok) add(&b, ",\"inputs\":null");
  else {
    add(&b, ",\"inputs\":{");
    add_u64s(&b, "ac_vrms_mv", (uint64_t)seg.v_mV, false);
    add_u64s(&b, "ac_irms_ma", (uint64_t)seg.i_mA, true);
    add_key(&b, "pf_assumed_millis", true); add_u32(&b, (uint32_t)kPfAssumedMillis);
    add_bool(&b, "ac_v_valid", true, true);
    add_bool(&b, "ac_i_valid", true, true);
    add(&b, "}");
  }
  add(&b, "}");
  (void)attr;
  (void)interval_present;
  (void)day;
  if (!b.ok || json_has_ambiguous_energy_key(json)) return false;

  uint64_t old_energy = durable_energy_;
  uint64_t old_residue = durable_residue_;
  uint64_t old_seq = seq_;
  uint32_t old_commits = commits_;
  uint32_t old_window_commits = commits_in_window_;
  bool old_tail = tail_open_;
  uint64_t old_alloc = allocated_;
  uint64_t old_unalloc = unallocated_;
  uint64_t old_cov = day_covered_ms_;
  uint32_t old_gaps = day_gaps_;
  bool old_measured = day_measured_;
  durable_energy_ = old_energy + published;
  durable_residue_ = residue_;
  tail_open_ = false;
  seq_ = old_seq + 1;
  commits_ = old_commits + 1;
  note_wear(mono_for_wear);
  if (seg.kind == RecordKind::Interval && attr == Attribution::Allocated) {
    allocated_ += seg.added_mwh;
    day_covered_ms_ += seg.covered_ms;
    day_measured_ = true;
  } else if (seg.kind == RecordKind::Interval && attr == Attribution::ValidZero) {
    day_covered_ms_ += seg.covered_ms;
    day_measured_ = true;
  } else if (seg.kind == RecordKind::Interval && attr == Attribution::Unallocated) {
    unallocated_ += seg.added_mwh;
    if (day_.valid) day_measured_ = true;
  } else if (seg.kind == RecordKind::Gap) {
    day_gaps_++;
  }
  if (!write_slot(old_seq, boot_, json) || !save_header(false) || !persist_image()) {
    durable_energy_ = old_energy;
    durable_residue_ = old_residue;
    seq_ = old_seq;
    commits_ = old_commits;
    commits_in_window_ = old_window_commits;
    tail_open_ = old_tail;
    allocated_ = old_alloc;
    unallocated_ = old_unalloc;
    day_covered_ms_ = old_cov;
    day_gaps_ = old_gaps;
    day_measured_ = old_measured;
    energy_ = durable_energy_ + (seg_.open ? seg_.added_mwh : 0);
    return false;
  }
  energy_ = durable_energy_;
  persisted_energy_ = durable_energy_;
  return true;
}

bool Meter::commit_segment(uint64_t mono_ms, bool) {
  if (!seg_.open) return true;
  if (seg_.covered_ms == 0 && seg_.added_mwh == 0) {
    seg_.open = false;
    tail_open_ = false;
    save_header(true);
    return true;
  }
  int64_t utc_end = 0;
  bool utc_end_known = utc_at(seg_.mono_end ? seg_.mono_end : mono_ms, &utc_end);
  CivilDay day;
  memset(&day, 0, sizeof(day));
  int64_t utc_start = 0;
  bool start_known = seg_.utc_start_known;
  if (start_known) utc_start = seg_.utc_start;
  if (start_known && utc_end_known && same_lagos_day(utc_start, utc_end)) lagos_day(utc_end, &day);
  else if (seg_.crossing) day.valid = false;
  bool ok = emit(seg_, utc_end, utc_end_known, day);
  if (ok) memset(&seg_, 0, sizeof(seg_));
  return ok;
}

bool Meter::queue_or_commit(const Segment &seg, int64_t utc_end, bool utc_end_known) {
  CivilDay day;
  memset(&day, 0, sizeof(day));
  if (utc_end_known && seg.utc_start_known && same_lagos_day(seg.utc_start, utc_end) && !seg.crossing)
    lagos_day(utc_end, &day);
  return emit(seg, utc_end, utc_end_known, day);
}

void Meter::start_segment(const AcSample &sample, bool, bool zero, RecordKind kind, UnavailableReason reason) {
  memset(&seg_, 0, sizeof(seg_));
  seg_.open = true;
  seg_.integrable = kind == RecordKind::Interval;
  seg_.valid_zero = zero;
  seg_.kind = kind;
  seg_.attribution = (!clock_.anchored || !coverage_allows(sample.mode, sample.coverage))
                         ? Attribution::Unallocated
                         : (zero ? Attribution::ValidZero : Attribution::Allocated);
  seg_.mono_start = sample.mono_ms;
  seg_.mono_end = sample.mono_ms;
  seg_.counter_start = durable_energy_;
  seg_.anchor_gen_start = clock_.generation;
  seg_.utc_start_known = utc_at(sample.mono_ms, &seg_.utc_start);
  seg_.v_mV = sample.v_rms_mV;
  seg_.i_mA = sample.i_rms_mA;
  seg_.pf_millis = last_pf_;
  seg_.mode = sample.mode;
  seg_.coverage = sample.coverage;
  seg_.source_version = sample.source_version ? sample.source_version : cfg_.source_version;
  safe_copy(seg_.source_path, sizeof(seg_.source_path), sample.source_path ? sample.source_path : "unknown");
  seg_.reason = reason;
  residue_ = 0;
  tail_open_ = true;
  if (!day_.valid && seg_.utc_start_known && clock_.anchored) {
    if (lagos_day(seg_.utc_start, &day_)) day_mono_start_ = sample.mono_ms;
  }
  save_header(true);
}

void Meter::close_for_clock_change(uint64_t mono_ms) { commit_segment(mono_ms, false); }

bool Meter::flush(uint64_t mono_ms) { return ready_ && commit_segment(mono_ms, false); }

bool Meter::set_pf_assumed(int32_t pf_millis, uint64_t mono_ms) {
  if (!ready_ || pf_millis != kPfAssumedMillis) return false;
  if (!commit_segment(mono_ms, false)) return false;
  cfg_.pf_assumed_millis = pf_millis;
  last_pf_ = pf_millis;
  return save_header(true);
}

bool Meter::on_sample(const AcSample &sample) {
  if (!ready_ || !counter_trusted_) return false;
  if (!sample.acquired) return false;
  if (sample.v_rms_mV < 0 || sample.i_rms_mA < 0) return false;
  if (sample.valid && sample.acquired && coverage_allows(sample.mode, sample.coverage)) {
    source_qualified_ = true;
    source_blocked_ = false;
    blocked_reason_ = UnavailableReason::None;
  } else if (sample.valid && sample.acquired) {
    source_qualified_ = true;
  }
  if (!have_last_) {
    last_ = sample;
    last_pf_ = cfg_.pf_assumed_millis;
    have_last_ = true;
    if (sample.valid && coverage_allows(sample.mode, sample.coverage))
      start_segment(sample, true, sample.v_rms_mV == 0 || sample.i_rms_mA == 0, RecordKind::Interval, UnavailableReason::None);
    return true;
  }
  if (sample.mono_ms < last_.mono_ms) return false;
  uint64_t dt = sample.mono_ms - last_.mono_ms;
  bool prev_ok = last_.valid && coverage_allows(last_.mode, last_.coverage);
  bool cur_ok = sample.valid && coverage_allows(sample.mode, sample.coverage);
  bool same_shape = prev_ok && cur_ok && last_.mode == sample.mode && last_.coverage == sample.coverage &&
                    last_.v_rms_mV == sample.v_rms_mV && last_.i_rms_mA == sample.i_rms_mA &&
                    last_pf_ == cfg_.pf_assumed_millis;
  int64_t u0 = 0, u1 = 0;
  bool k0 = prev_ok && utc_at(last_.mono_ms, &u0);
  bool k1 = cur_ok && utc_at(sample.mono_ms, &u1);
  bool end_is_exact_midnight = false;
  if (k1) {
    int64_t local_end = u1 + kLagosOffsetSeconds;
    end_is_exact_midnight = local_end > 0 && local_end % 86400 == 0;
  }
  bool cross = k0 && k1 && !same_lagos_day(u0, end_is_exact_midnight ? u1 - 1 : u1);
  bool too_long = dt > cfg_.max_integrate_ms;
  bool discontinuity = !prev_ok || !cur_ok || too_long || last_.mode != sample.mode ||
                       last_.coverage != sample.coverage || !sample.valid || !last_.valid;

  if (dt > 0 && discontinuity) {
    if (!commit_segment(last_.mono_ms, false)) return false;
    Segment gap;
    memset(&gap, 0, sizeof(gap));
    gap.kind = RecordKind::Gap;
    gap.attribution = Attribution::Unallocated;
    gap.mono_start = last_.mono_ms;
    gap.mono_end = sample.mono_ms;
    gap.counter_start = durable_energy_;
    gap.reason = !sample.valid || !last_.valid ? UnavailableReason::SampleInvalid : uncover_reason(prev_ok ? sample : last_);
    if (!last_.valid || !sample.valid) gap.reason = UnavailableReason::SampleInvalid;
    else if (!prev_ok) gap.reason = uncover_reason(last_);
    else if (!cur_ok) gap.reason = uncover_reason(sample);
    else if (too_long) gap.reason = UnavailableReason::SampleMissing;
    else gap.reason = uncover_reason(sample);
    gap.mode = sample.mode;
    gap.coverage = sample.coverage;
    gap.pf_millis = last_pf_;
    gap.source_version = sample.source_version;
    safe_copy(gap.source_path, sizeof(gap.source_path), sample.source_path);
    gap.anchor_gen_start = clock_.generation;
    if (!queue_or_commit(gap, 0, false)) return false;
    last_ = sample;
    last_pf_ = cfg_.pf_assumed_millis;
    if (cur_ok) start_segment(sample, true, sample.v_rms_mV == 0 || sample.i_rms_mA == 0, RecordKind::Interval, UnavailableReason::None);
    return true;
  }

  if (dt == 0) {
    last_ = sample;
    return true;
  }

  bool zero = last_.v_rms_mV == 0 || last_.i_rms_mA == 0;
  uint64_t added = 0;
  uint64_t residue_before = residue_;
  uint64_t residue_next = residue_;
  if (!zero) {
    if (!estimated_mwh(last_.v_rms_mV, last_.i_rms_mA, last_pf_, dt, &residue_next, &added)) return false;
  }
  bool rolled_day = false;
  if (seg_.open && seg_.utc_start_known && k1) {
    CivilDay opened, step_day;
    int64_t step_utc = end_is_exact_midnight ? u1 - 1 : u1;
    if (lagos_day(seg_.utc_start, &opened) && lagos_day(step_utc, &step_day))
      rolled_day = opened.year != step_day.year || opened.month != step_day.month || opened.day != step_day.day;
  }
  if (cross || rolled_day || !same_shape) {
    if (!commit_segment(last_.mono_ms, false)) return false;
    residue_ = residue_next;
    Segment step;
    memset(&step, 0, sizeof(step));
    step.kind = RecordKind::Interval;
    step.crossing = cross;
    step.valid_zero = zero;
    step.added_mwh = added;
    step.covered_ms = (uint32_t)dt;
    step.mono_start = last_.mono_ms;
    step.mono_end = sample.mono_ms;
    step.counter_start = durable_energy_;
    step.v_mV = last_.v_rms_mV;
    step.i_mA = last_.i_rms_mA;
    step.pf_millis = last_pf_;
    step.mode = last_.mode;
    step.coverage = last_.coverage;
    step.source_version = last_.source_version;
    safe_copy(step.source_path, sizeof(step.source_path), last_.source_path);
    step.anchor_gen_start = clock_.generation;
    step.utc_start_known = k0;
    step.utc_start = u0;
    step.reason = UnavailableReason::None;
    if (cross || !k0 || !k1) step.attribution = Attribution::Unallocated;
    else if (zero) step.attribution = Attribution::ValidZero;
    else step.attribution = Attribution::Allocated;
    CivilDay day;
    memset(&day, 0, sizeof(day));
    if (!cross && k0 && k1) lagos_day(u1, &day);
    if (!emit(step, u1, k1, day)) {
      residue_ = residue_before;
      return false;
    }
    if (cross && day_.valid) {
      if (!checkpoint_from_buckets(sample.mono_ms)) {
        last_ = sample;
        last_pf_ = cfg_.pf_assumed_millis;
        return false;
      }
    }
    last_ = sample;
    last_pf_ = cfg_.pf_assumed_millis;
    if (cur_ok) start_segment(sample, true, sample.v_rms_mV == 0 || sample.i_rms_mA == 0, RecordKind::Interval, UnavailableReason::None);
    return true;
  }

  if (!seg_.open) start_segment(last_, true, zero, RecordKind::Interval, UnavailableReason::None);
  residue_ = residue_next;
  seg_.added_mwh += added;
  seg_.covered_ms += (uint32_t)dt;
  seg_.mono_end = sample.mono_ms;
  seg_.valid_zero = zero;
  if (zero) seg_.attribution = clock_.anchored ? Attribution::ValidZero : Attribution::Unallocated;
  else if (!clock_.anchored) seg_.attribution = Attribution::Unallocated;
  energy_ = durable_energy_ + seg_.added_mwh;
  last_ = sample;
  last_pf_ = cfg_.pf_assumed_millis;
  if (cfg_.commit_interval_ms && seg_.covered_ms >= cfg_.commit_interval_ms) {
    if (!commit_segment(sample.mono_ms, false)) return false;
    if (cur_ok) start_segment(sample, true, sample.v_rms_mV == 0 || sample.i_rms_mA == 0, RecordKind::Interval, UnavailableReason::None);
  }
  return true;
}

bool Meter::observe_rtc(const uint8_t first[7], const uint8_t second[7], uint64_t mono_ms) {
  if (!ready_) return false;
  RtcReading rd;
  if (!ds1307_pair(first, second, &rd) || !rd.ok) {
    if (decode_ds1307(first).halted || decode_ds1307(second).halted) {
      close_for_clock_change(mono_ms);
      clock_.anchored = false;
      clock_.quality = TimeQuality::Uncertain;
      clock_.generation++;
    }
    return false;
  }
  if (!clock_.have_face) {
    clock_.have_face = true;
    clock_.last_unix = rd.unix_utc;
    clock_.last_mono = mono_ms;
    if (!clock_.anchored) clock_.quality = TimeQuality::Unanchored;
    return true;
  }
  int64_t expected = clock_.last_unix;
  if (mono_ms > clock_.last_mono) expected += (int64_t)((mono_ms - clock_.last_mono) / 1000ull);
  int64_t delta = rd.unix_utc - expected;
  if (delta < 0) delta = -delta;
  if (delta > 3) {
    close_for_clock_change(mono_ms);
    clock_.anchored = false;
    clock_.quality = TimeQuality::Adjusted;
    clock_.adjustments++;
    clock_.generation++;
    clock_.last_unix = rd.unix_utc;
    clock_.last_mono = mono_ms;
    return true;
  }
  if (!clock_.anchored && mono_ms >= clock_.last_mono + 1000 && rd.unix_utc >= clock_.last_unix &&
      rd.unix_utc - clock_.last_unix <= 3)
    clock_.quality = TimeQuality::PlausibleTick;
  clock_.last_unix = rd.unix_utc;
  clock_.last_mono = mono_ms;
  clock_.have_face = true;
  return true;
}

bool Meter::agree_anchor(int64_t rtc_unix, int64_t external_unix, uint64_t mono_ms, int64_t tolerance_s) {
  if (!ready_ || tolerance_s < 0) return false;
  int64_t d = rtc_unix - external_unix;
  if (d < 0) d = -d;
  if (d > tolerance_s) return false;
  bool was = clock_.anchored;
  int64_t predicted = 0;
  bool have_pred = was && utc_at(mono_ms, &predicted);
  close_for_clock_change(mono_ms);
  if (have_pred) {
    int64_t jump = predicted - external_unix;
    if (jump < 0) jump = -jump;
    if (jump > tolerance_s) clock_.adjustments++;
  }
  CivilDay old = day_;
  if (was && have_pred && old.valid) {
    CivilDay neu;
    if (lagos_day(external_unix, &neu) && (neu.year != old.year || neu.month != old.month || neu.day != old.day)) {
      if (!checkpoint_from_buckets(mono_ms)) return false;
    }
  } else if (!was) {
    allocated_ = 0;
    unallocated_ = 0;
    day_covered_ms_ = 0;
    day_gaps_ = 0;
    day_measured_ = false;
    day_.valid = false;
  }
  clock_.anchored = true;
  clock_.quality = TimeQuality::Anchored;
  clock_.utc_anchor = external_unix;
  clock_.mono_anchor = mono_ms;
  clock_.generation++;
  clock_.have_face = true;
  clock_.last_unix = rtc_unix;
  clock_.last_mono = mono_ms;
  if (!day_.valid) {
    if (lagos_day(external_unix, &day_)) day_mono_start_ = mono_ms;
  }
  save_header(true);
  return true;
}

bool Meter::reject_stale_nvs(int64_t) {
  if (!ready_) return false;
  close_for_clock_change(0);
  clock_.anchored = false;
  clock_.quality = TimeQuality::StaleNvsRejected;
  clock_.generation++;
  return false;
}

bool Meter::checkpoint_from_buckets(uint64_t mono_ms) {
  if (!day_.valid) {
    int64_t utc = 0;
    if (utc_at(mono_ms, &utc)) {
      if (lagos_day(utc, &day_)) day_mono_start_ = day_mono_start_ ? day_mono_start_ : mono_ms;
    }
    return day_.valid;
  }
  if (!commit_segment(mono_ms, true)) return false;
  Segment cp;
  memset(&cp, 0, sizeof(cp));
  cp.kind = RecordKind::DailyCheckpoint;
  cp.mono_start = day_mono_start_ ? day_mono_start_ : mono_ms;
  cp.mono_end = mono_ms;
  cp.counter_start = durable_energy_;
  cp.added_mwh = 0;
  cp.covered_ms = (uint32_t)day_covered_ms_;
  cp.pf_millis = cfg_.pf_assumed_millis;
  cp.mode = Mode::Unknown;
  cp.coverage = Coverage::Unknown;
  cp.attribution = Attribution::Unallocated;
  cp.reason = day_measured_ ? UnavailableReason::None : blocked_reason_;
  cp.anchor_gen_start = clock_.generation;
  cp.source_version = cfg_.source_version;
  safe_copy(cp.source_path, sizeof(cp.source_path), "daily_checkpoint");
  bool present_alloc = day_measured_;
  uint64_t alloc = allocated_;
  uint64_t unalloc = unallocated_;
  char json[1200];
  Buf b = {json, 0, sizeof(json), true};
  add(&b, "{");
  add_str(&b, "contract", kContract, false);
  add_str(&b, "record_kind", "daily_checkpoint", true);
  add_str(&b, "device_id", device_id_, true);
  add_str(&b, "stream", kStream, true);
  add_u64s(&b, "meter_epoch", epoch_, true);
  add_u64s(&b, "boot_id", boot_, true);
  add_u64s(&b, "seq", seq_, true);
  add_u64s(&b, "monotonic_start_ms", cp.mono_start, true);
  add_u64s(&b, "monotonic_end_ms", cp.mono_end, true);
  add_key(&b, "utc_start", true); add(&b, "null");
  add_key(&b, "utc_end", true); add(&b, "null");
  add_u64s(&b, "counter_start_mwh", durable_energy_, true);
  add_u64s(&b, "counter_end_mwh", durable_energy_, true);
  add_key(&b, "interval_estimated_mwh", true); add(&b, "null");
  add_null_or_u64(&b, "allocated_estimated_mwh", present_alloc, alloc, true);
  add_null_or_u64(&b, "unallocated_estimated_mwh", present_alloc, unalloc, true);
  add_key(&b, "covered_ms", true); add_u32(&b, cp.covered_ms);
  uint32_t interval_ms = cp.mono_end >= cp.mono_start ? (uint32_t)(cp.mono_end - cp.mono_start) : 0;
  add_key(&b, "interval_ms", true); add_u32(&b, interval_ms);
  add_str(&b, "time_quality", quality_name(clock_.quality), true);
  add_str(&b, "mode", "unknown", true);
  add_str(&b, "coverage_scope", "unknown", true);
  const char *attr = "unallocated";
  if (present_alloc && day_gaps_ == 0 && unalloc == 0 && alloc == 0 && day_covered_ms_ > 0) attr = "valid_zero";
  else if (present_alloc && day_gaps_ == 0 && unalloc == 0) attr = "allocated";
  add_str(&b, "attribution", attr, true);
  add_u64s(&b, "model_revision", cfg_.model_revision, true);
  add_u64s(&b, "source_version", cfg_.source_version, true);
  add_str(&b, "source_path", "daily_checkpoint", true);
  add_key(&b, "pf_assumed_millis", true); add_u32(&b, (uint32_t)cfg_.pf_assumed_millis);
  add_bool(&b, "pf_is_assumption", true, true);
  bool available = present_alloc && day_gaps_ == 0 && unalloc == 0;
  add_bool(&b, "estimate_available", available, true);
  add_str(&b, "unavailable_reason", available ? "none" : (present_alloc ? "sample_missing" : reason_name(blocked_reason_)), true);
  char daybuf[16];
  snprintf(daybuf, sizeof(daybuf), "%04d-%02d-%02d", day_.year, day_.month, day_.day);
  add_str(&b, "civil_day", daybuf, true);
  add_str(&b, "timezone", "Africa/Lagos", true);
  add_key(&b, "utc_offset_s", true); add_u32(&b, (uint32_t)kLagosOffsetSeconds);
  add_bool(&b, "synthetic", false, true);
  add_str(&b, "transport", "pending_contract", true);
  add_key(&b, "loss_events", true); add_u32(&b, loss_);
  add_bool(&b, "overflow_alarm", overflow_, true);
  add_bool(&b, "bms_dc_used", false, true);
  add_key(&b, "v_rms_mV", true); add(&b, "null");
  add_key(&b, "i_rms_mA", true); add(&b, "null");
  add(&b, "}");
  if (!b.ok || json_has_ambiguous_energy_key(json)) return false;
  CivilDay closed = day_;
  (void)closed;
  uint64_t keep_alloc = allocated_;
  uint64_t keep_un = unallocated_;
  uint64_t keep_cov = day_covered_ms_;
  uint32_t keep_gaps = day_gaps_;
  bool keep_meas = day_measured_;
  CivilDay keep_day = day_;
  (void)json;
  allocated_ = 0;
  unallocated_ = 0;
  day_covered_ms_ = 0;
  day_gaps_ = 0;
  day_measured_ = false;
  day_.valid = false;
  day_mono_start_ = 0;
  if (!save_header(true)) {
    allocated_ = keep_alloc;
    unallocated_ = keep_un;
    day_covered_ms_ = keep_cov;
    day_gaps_ = keep_gaps;
    day_measured_ = keep_meas;
    day_ = keep_day;
    return false;
  }
  return true;
}

bool Meter::poll_day(uint64_t mono_ms) {
  if (!ready_ || !clock_.anchored || !day_.valid) return false;
  int64_t utc = 0;
  if (!utc_at(mono_ms, &utc)) return false;
  CivilDay now;
  if (!lagos_day(utc, &now)) return false;
  if (now.year == day_.year && now.month == day_.month && now.day == day_.day) return false;
  if (!checkpoint_from_buckets(mono_ms)) return false;
  if (lagos_day(utc, &day_)) day_mono_start_ = mono_ms;
  return save_header(true);
}

MeterStatus Meter::status() const {
  MeterStatus s;
  memset(&s, 0, sizeof(s));
  s.estimate_available = source_qualified_ && !source_blocked_ && counter_trusted_;
  s.counter_trusted = counter_trusted_;
  s.journal_untrusted = journal_untrusted_;
  s.overflow_alarm = overflow_;
  s.transport_enabled = false;
  s.synthetic = false;
  s.tail_open = tail_open_ || (seg_.open && seg_.added_mwh > 0);
  s.durable = ready_;
  s.reason = source_blocked_ ? blocked_reason_ : UnavailableReason::None;
  s.time_quality = clock_.quality;
  s.counter_mwh = durable_energy_ + (seg_.open ? seg_.added_mwh : 0);
  s.persisted_mwh = durable_energy_;
  s.residue = residue_;
  s.meter_epoch = epoch_;
  s.boot_id = boot_;
  s.next_seq = seq_;
  s.loss_events = loss_;
  s.adjustments = clock_.adjustments;
  s.allocated_mwh = allocated_;
  s.unallocated_mwh = unallocated_;
  s.civil_day = day_;
  s.pf_assumed_millis = cfg_.pf_assumed_millis;
  s.model_revision = cfg_.model_revision;
  s.source_version = cfg_.source_version;
  s.pending_ram = (seg_.open && seg_.added_mwh > 0) ? 1u : 0u;
  uint32_t n = 0;
  char dump[kDefaultSlotBytes];
  while (n < 64 && record_json(n, dump, sizeof(dump))) n++;
  s.committed_records = n;
  safe_copy(s.device_id, sizeof(s.device_id), device_id_);
  s.civil_text[0] = 0;
  if (day_.valid) snprintf(s.civil_text, sizeof(s.civil_text), "%04d-%02d-%02d", day_.year, day_.month, day_.day);
  s.interval_present = false;
  s.allocated_present = false;
  s.attribution = Attribution::Unallocated;
  if (!s.estimate_available) {
    s.attribution = Attribution::Unallocated;
  } else if (day_measured_ && day_gaps_ == 0 && unallocated_ == 0 && allocated_ == 0 && day_covered_ms_ > 0) {
    s.attribution = Attribution::ValidZero;
    s.interval_present = true;
    s.interval_mwh = 0;
    s.allocated_present = true;
    s.allocated_report_mwh = 0;
  } else if (day_measured_ && day_gaps_ == 0 && unallocated_ == 0) {
    s.attribution = Attribution::Allocated;
    s.interval_present = true;
    s.interval_mwh = allocated_;
    s.allocated_present = true;
    s.allocated_report_mwh = allocated_;
  } else if (day_measured_) {
    s.attribution = Attribution::Unallocated;
    s.allocated_present = true;
    s.allocated_report_mwh = allocated_;
  }
  return s;
}

uint32_t status_json(const MeterStatus &status, const char *, bool, char *dst, uint32_t cap) {
  if (!dst || cap < 32) return 0;
  Buf b = {dst, 0, cap, true};
  char boot_text[16];
  snprintf(boot_text, sizeof(boot_text), "%u", status.boot_id);
  const char *tq = "absent";
  if (status.time_quality == TimeQuality::Anchored) tq = "rtc_ds1307";
  else if (status.time_quality != TimeQuality::Uncertain) tq = "uncertain";
  add(&b, "{");
  add_str(&b, "contract", kContract, false);
  add_bool(&b, "simulation", false, true);
  add_str(&b, "device_id", status.device_id[0] ? status.device_id : "unknown", true);
  add_str(&b, "stream", kStream, true);
  add_u64s(&b, "meter_epoch", status.meter_epoch, true);
  add_str(&b, "boot_id", boot_text, true);
  add_u64s(&b, "seq", status.next_seq ? status.next_seq : 1, true);
  add_key(&b, "clock_correction_version", true); add_u32(&b, (uint32_t)status.adjustments);
  add(&b, ",\"model\":{");
  add_str(&b, "revision", kContract, false);
  add_str(&b, "source_path", kModelSourcePath, true);
  add_str(&b, "version", kModelWireVersion, true);
  add_str(&b, "pf_kind", "assumed", true);
  add_key(&b, "pf_assumption_millis", true); add_u32(&b, (uint32_t)kPfAssumedMillis);
  add_str(&b, "coverage_scope", "unknown", true);
  add(&b, "}");
  add_u64s(&b, "monotonic_start_ms", 0, true);
  add_u64s(&b, "monotonic_end_ms", 0, true);
  add_key(&b, "utc_start", true); add(&b, "null");
  add_key(&b, "utc_end", true); add(&b, "null");
  add_str(&b, "time_quality", tq, true);
  add_u64s(&b, "counter_start_mwh", status.counter_trusted ? status.persisted_mwh : 0, true);
  add_u64s(&b, "counter_end_mwh", status.counter_trusted ? status.persisted_mwh : 0, true);
  add_u64s(&b, "interval_estimated_mwh", 0, true);
  add_u64s(&b, "covered_ms", 0, true);
  add_u64s(&b, "interval_ms", 0, true);
  add_str(&b, "sample_validity", "absent", true);
  add(&b, ",\"source_mode_coverage\":{");
  add_u64s(&b, "inverter_output_only_ms", 0, false);
  add_u64s(&b, "common_output_ms", 0, true);
  add_u64s(&b, "unknown_ms", 0, true);
  add(&b, "},\"inputs\":null}");
  if (!b.ok) return 0;
  return b.n;
}

}  // namespace loadac
