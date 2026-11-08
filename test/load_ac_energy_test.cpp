#include "load_ac_energy.h"
#include "original_v01_profile.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

using namespace loadac;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (cond)                                                                       \
      ++g_pass;                                                                     \
    else {                                                                          \
      ++g_fail;                                                                     \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
    }                                                                               \
  } while (0)

static const char *json_value(const char *json, const char *key) {
  char pat[96];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(json, pat);
  return p ? p + strlen(pat) : 0;
}

static bool json_str(const char *json, const char *key, char *out, size_t n) {
  const char *p = json_value(json, key);
  if (!p || *p != '"' || n == 0) return false;
  ++p;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
  out[i] = 0;
  return *p == '"';
}

static bool json_is_null(const char *json, const char *key) {
  const char *p = json_value(json, key);
  return p && strncmp(p, "null", 4) == 0;
}

static bool json_u64(const char *json, const char *key, uint64_t *out) {
  char buf[32];
  if (!json_str(json, key, buf, sizeof(buf))) return false;
  char *end = 0;
  unsigned long long v = strtoull(buf, &end, 10);
  if (!end || *end) return false;
  *out = (uint64_t)v;
  return true;
}

static bool json_bool(const char *json, const char *key, bool *out) {
  const char *p = json_value(json, key);
  if (!p) return false;
  if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
  if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

static bool json_i32(const char *json, const char *key, int32_t *out) {
  const char *p = json_value(json, key);
  if (!p || *p == '"') return false;
  char *end = 0;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  *out = (int32_t)v;
  return true;
}

static ModelConfig test_cfg() {
  ModelConfig c = default_model();
  c.max_integrate_ms = 4000000;
  c.max_commits_per_window = 1000;
  c.commit_interval_ms = 100000000;
  return c;
}

static AcSample sample_at(uint64_t t, int32_t v, int32_t i) {
  AcSample s;
  memset(&s, 0, sizeof(s));
  s.mono_ms = t;
  s.acquired = true;
  s.valid = true;
  s.v_rms_mV = v;
  s.i_rms_mA = i;
  s.mode = Mode::Inverter;
  s.coverage = Coverage::InverterOutputOnly;
  s.source_version = 1;
  s.source_path = "stm32_explicit_vrms_irms";
  return s;
}

static void test_crc_and_calendar() {
  const uint8_t msg[] = "123456789";
  CHECK(crc32_ieee_bytes(msg, 9) == 0xCBF43926u);
  CHECK(is_leap_year(2000) && is_leap_year(2024) && !is_leap_year(1900) && !is_leap_year(2025));
  CHECK(civil_valid(2024, 2, 29) && !civil_valid(2025, 2, 29) && !civil_valid(2024, 2, 30));
  CivilDay d;
  int64_t lagos_march = unix_from_civil_utc(2024, 2, 29, 23, 0, 0);
  CHECK(lagos_day(lagos_march, &d) && d.year == 2024 && d.month == 3 && d.day == 1);
  CHECK(lagos_day(lagos_march - 1, &d) && d.year == 2024 && d.month == 2 && d.day == 29);
  int64_t not_leap = unix_from_civil_utc(2025, 2, 28, 23, 0, 0);
  CHECK(lagos_day(not_leap, &d) && d.year == 2025 && d.month == 3 && d.day == 1);
  CHECK(lagos_day(not_leap - 1, &d) && d.month == 2 && d.day == 28);
  int64_t dec = unix_from_civil_utc(2026, 11, 30, 23, 0, 0);
  CHECK(lagos_day(dec, &d) && d.year == 2026 && d.month == 12 && d.day == 1);
  CHECK(lagos_day(82800, &d) && d.year == 1970 && d.month == 1 && d.day == 2);
  CHECK(lagos_day(82799, &d) && d.day == 1);
  CHECK(!ds1307_has_osf_bit());
}

static void test_ds1307_and_backoff() {
  uint8_t a[7], b[7];
  encode_ds1307(2024, 2, 29, 13, 4, 5, a);
  RtcReading rd = decode_ds1307(a);
  CHECK(rd.ok && rd.unix_utc == unix_from_civil_utc(2024, 2, 29, 13, 4, 5));
  encode_ds1307(2025, 2, 29, 0, 0, 0, a);
  CHECK(!decode_ds1307(a).ok);
  a[0] = 0x29; a[1] = 0; a[2] = 0; a[3] = 1; a[4] = 0x29; a[5] = 0x02; a[6] = 0x25;
  CHECK(!decode_ds1307(a).ok);
  encode_ds1307(2024, 2, 29, 1, 2, 3, a);
  a[0] |= 0x80;
  CHECK(decode_ds1307(a).halted && !decode_ds1307(a).ok);
  encode_ds1307(2024, 2, 29, 1, 2, 3, a);
  a[0] = 0x0A;
  CHECK(!decode_ds1307(a).ok);
  encode_ds1307(2024, 6, 15, 14, 30, 45, b);
  b[2] = (uint8_t)(0x40 | 0x20 | 0x02);
  b[1] = 0x30;
  b[0] = 0x45;
  rd = decode_ds1307(b);
  CHECK(rd.ok && rd.unix_utc == unix_from_civil_utc(2024, 6, 15, 14, 30, 45));
  encode_ds1307(2024, 6, 15, 14, 30, 45, a);
  encode_ds1307(2024, 6, 15, 14, 30, 46, b);
  RtcReading pair;
  CHECK(ds1307_pair(a, b, &pair) && pair.ok && pair.unix_utc == unix_from_civil_utc(2024, 6, 15, 14, 30, 46));
  encode_ds1307(2024, 6, 15, 14, 30, 55, b);
  CHECK(!ds1307_pair(a, b, &pair));
  b[0] |= 0x80;
  CHECK(!ds1307_pair(a, b, &pair));

  I2cBackoff bo;
  memset(&bo, 0, sizeof(bo));
  CHECK(i2c_allow(bo, 0));
  i2c_fail(&bo, 0);
  CHECK(bo.backoff_ms == 100 && !i2c_allow(bo, 50) && i2c_allow(bo, 100));
  i2c_fail(&bo, 100);
  CHECK(bo.backoff_ms == 200);
  i2c_fail(&bo, 100);
  CHECK(bo.backoff_ms == 400);
  for (int i = 0; i < 20; ++i) i2c_fail(&bo, 0);
  CHECK(bo.backoff_ms == 60000);
  i2c_ok(&bo);
  CHECK(bo.failures == 0 && i2c_allow(bo, 0) && bo.backoff_ms == 100);
}

static void test_arithmetic() {
  uint64_t residue = 0, added = 0;
  CHECK(estimated_mwh(230000, 1000, 1000, 3600000, &residue, &added));
  CHECK(added == 230000 && residue == 0);
  residue = 0;
  uint64_t total = 0;
  for (int i = 0; i < 3600; ++i) {
    CHECK(estimated_mwh(1000, 1000, 1000, 1, &residue, &added));
    total += added;
  }
  CHECK(total == 1 && residue == 0);
  uint64_t residue2 = 0, one = 0;
  CHECK(estimated_mwh(1000, 1000, 1000, 3600, &residue2, &one));
  CHECK(one == 1 && residue2 == 0);
  CHECK(!estimated_mwh(1, 1, 0, 1, &residue2, &one));
  CHECK(!estimated_mwh(-1, 1, 1000, 1, &residue2, &one));
  residue = 0;
  CHECK(estimated_mwh(1000, 3600000, 1000, 1000, &residue, &added) && added == 1000 && residue == 0);
  CHECK(estimated_mwh(1000, 3600000, 500, 1000, &residue, &added) && added == 500 && residue == 0);
  ModelConfig defaults = default_model();
  CHECK(defaults.pf_assumed_millis == 800 && defaults.model_revision == 2);
  residue = 0;
  CHECK(estimated_mwh(230000, 1000, kPfAssumedMillis, 3600000, &residue, &added));
  CHECK(added == 184000 && residue == 0);
  // A metered watt-hour is not an input. 1000 mWh from a PZEM stays 1000;
  // this estimator does not multiply it by 0.8.
  CHECK(kPfAssumedMillis == 800);
}

static void test_source_qualification() {
  UnavailableReason why = UnavailableReason::None;
  Stm32Observation pinned = pinned_main_stm32_observation(10);
  pinned.vout_mV = 230000;
  pinned.iout_mA = 1000;
  AcSample s = qualify_stm32(pinned, &why);
  CHECK(!s.acquired && why == UnavailableReason::Stm32TextVoutIoutNotRms);
  pinned.miss_forced_zero = true;
  s = qualify_stm32(pinned, &why);
  CHECK(!s.acquired && why == UnavailableReason::MissForcedZero);
  Stm32Observation good = pinned_main_stm32_observation(11);
  good.has_vrms = good.has_irms = true;
  good.vrms_valid = good.irms_valid = true;
  good.vrms_mV = 230000;
  good.irms_mA = 1000;
  good.has_mode = good.has_coverage = true;
  good.mode = Mode::Inverter;
  good.coverage = Coverage::InverterOutputOnly;
  good.source_path = "stm32_explicit_vrms_irms";
  s = qualify_stm32(good, &why);
  CHECK(s.acquired && s.valid && s.v_rms_mV == 230000 && s.i_rms_mA == 1000 && why == UnavailableReason::None);
  good.vrms_valid = false;
  s = qualify_stm32(good, &why);
  CHECK(!s.acquired && why == UnavailableReason::RmsValidityFalse);
  good.vrms_valid = true;
  good.has_coverage = false;
  s = qualify_stm32(good, &why);
  CHECK(!s.acquired && why == UnavailableReason::CoverageUnknown);
  good.has_coverage = true;
  good.coverage = Coverage::InverterOutputOnly;
  good.mode = Mode::GridBypass;
  s = qualify_stm32(good, &why);
  CHECK(s.acquired && s.valid && why == UnavailableReason::BypassNotCovered);
  CHECK(!coverage_allows(Mode::GridBypass, Coverage::InverterOutputOnly));
  CHECK(coverage_allows(Mode::GridBypass, Coverage::CommonOutput));
  CHECK(coverage_allows(Mode::Inverter, Coverage::InverterOutputOnly));
  AcSample bms = reject_bms_dc(1);
  AcSample ads = reject_ads_pv(1);
  CHECK(!bms.acquired && !ads.acquired);
  CHECK(strcmp(bms.source_path, "jk_bms_dc_discharge") == 0);
  CHECK(strcmp(ads.source_path, "ads1115_dc_pv_bus") == 0);

  CHECK(ORIGINAL_V01_INVERTER_RX == 33 && ORIGINAL_V01_INVERTER_TX == 2);
  CHECK(ORIGINAL_V01_BMS_RX == 17 && ORIGINAL_V01_BMS_TX == 16);
  CHECK(ORIGINAL_V01_RELAY == 15 && ORIGINAL_V01_LIGHTS == 25 && ORIGINAL_V01_RELAY != ORIGINAL_V01_LIGHTS);
  CHECK(ORIGINAL_V01_SD_CS < 0 && ORIGINAL_V01_SD_CS != ORIGINAL_V01_INVERTER_RX);
  CHECK(ORIGINAL_V01_I2C_SDA == 21 && ORIGINAL_V01_I2C_SCL == 22 && ORIGINAL_V01_I2C_HZ == 100000);
  CHECK(ORIGINAL_V01_LORA_CS == 4 && ORIGINAL_V01_LORA_RST == 26 && ORIGINAL_V01_LORA_DIO0 == 2);
  CHECK(ORIGINAL_V01_DS1307_ADDR == 0x68 && ORIGINAL_V01_INVERTER_BAUD == 9600);
  CHECK(strstr(kMissingPath, "VOUT") && strstr(kMissingPath, "finalizeRecord") && strstr(kMissingPath, "energy_update"));
}

static bool open_meter(Meter &m, MemoryStore &store, const ModelConfig &cfg) {
  if (!m.begin(store, cfg)) return false;
  return m.set_device_id("001122334455");
}

static void test_meter_energy_and_journal() {
  MemoryStore store;
  Meter meter;
  ModelConfig cfg = test_cfg();
  CHECK(open_meter(meter, store, cfg));
  CHECK(meter.status().pf_assumed_millis == 800);
  CHECK(meter.status().model_revision == 2);
  CHECK(!meter.note_http_success(1, 1));
  CHECK(!meter.status().transport_enabled);
  CHECK(!meter.status().estimate_available);
  AcSample bms = reject_bms_dc(0);
  CHECK(!meter.on_sample(bms));
  CHECK(meter.status().counter_mwh == 0);

  CHECK(meter.on_sample(sample_at(0, 230000, 1000)));
  CHECK(meter.on_sample(sample_at(3600000, 230000, 1000)));
  CHECK(meter.status().counter_mwh == 184000);
  CHECK(meter.status().residue == 0);
  CHECK(meter.flush(3600000));
  CHECK(meter.status().persisted_mwh == 184000);
  char rec[1200];
  CHECK(meter.record_json(0, rec, sizeof(rec)));
  uint64_t mwh = 0;
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 184000);
  CHECK(json_u64(rec, "counter_end_mwh", &mwh) && mwh == 184000);
  CHECK(json_u64(rec, "counter_start_mwh", &mwh) && mwh == 0);
  int32_t pf = 0, mv = 0, ma = 0;
  CHECK(json_i32(rec, "pf_assumed_millis", &pf) && pf == 800);
  char prov[64], model[8];
  CHECK(json_str(rec, "assumption_provenance", prov, sizeof(prov)) && strcmp(prov, kAssumptionProvenance) == 0);
  CHECK(json_str(rec, "model_revision", model, sizeof(model)) && strcmp(model, "2") == 0);
  CHECK(json_i32(rec, "v_rms_mV", &mv) && mv == 230000);
  CHECK(json_i32(rec, "i_rms_mA", &ma) && ma == 1000);
  bool flag = false;
  CHECK(json_bool(rec, "pf_is_assumption", &flag) && flag);
  CHECK(json_bool(rec, "synthetic", &flag) && !flag);
  CHECK(json_bool(rec, "bms_dc_used", &flag) && !flag);
  CHECK(!json_has_ambiguous_energy_key(rec));
  char transport[32];
  CHECK(json_str(rec, "transport", transport, sizeof(transport)) && strcmp(transport, "pending_contract") == 0);
  CHECK(json_str(rec, "contract", transport, sizeof(transport)) && strcmp(transport, kContract) == 0);

  CHECK(!meter.set_pf_assumed(0, 3600000));
  CHECK(!meter.set_pf_assumed(1001, 3600000));
  CHECK(meter.set_pf_assumed(500, 3600000));
  CHECK(meter.on_sample(sample_at(3600000, 1000, 3600000)));
  CHECK(meter.on_sample(sample_at(3601000, 1000, 3600000)));
  CHECK(meter.status().counter_mwh == 184500);
  CHECK(meter.flush(3601000));
  CHECK(meter.record_json(1, rec, sizeof(rec)));
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 500);
  CHECK(json_i32(rec, "pf_assumed_millis", &pf) && pf == 500);
  CHECK(json_str(rec, "assumption_provenance", prov, sizeof(prov)) && strcmp(prov, "explicit-pf-assumption") == 0);
  CHECK(json_str(rec, "model_revision", model, sizeof(model)) && strcmp(model, "2") == 0);

  MemoryStore reboot;
  store.clone_durable(reboot);
  Meter again;
  CHECK(again.begin(reboot, cfg));
  CHECK(again.status().boot_id == 2);
  CHECK(again.status().meter_epoch == 1);
  CHECK(again.status().persisted_mwh == 184500);
  CHECK(again.status().pf_assumed_millis == 500);
  CHECK(again.status().next_seq == meter.status().next_seq);
  char id[20];
  CHECK(json_str(rec, "device_id", id, sizeof(id)));
  char rec2[1200];
  CHECK(again.record_json(0, rec2, sizeof(rec2)));
  CHECK(strcmp(rec2, rec) != 0 || json_u64(rec2, "interval_estimated_mwh", &mwh));
  CHECK(again.record_json(0, rec2, sizeof(rec2)));
  CHECK(json_u64(rec2, "interval_estimated_mwh", &mwh) && mwh == 184000);
  CHECK(json_str(rec2, "device_id", id, sizeof(id)) && strcmp(id, "001122334455") == 0);
}

static void test_gaps_zero_bypass() {
  MemoryStore store;
  Meter meter;
  ModelConfig cfg = test_cfg();
  cfg.max_integrate_ms = 2000;
  CHECK(open_meter(meter, store, cfg));
  int64_t utc = unix_from_civil_utc(2026, 11, 8, 12, 0, 0);
  CHECK(meter.agree_anchor(utc, utc, 1000, 2));
  CHECK(meter.on_sample(sample_at(1000, 230000, 1000)));
  CHECK(meter.on_sample(sample_at(1000 + 10000, 230000, 1000)));
  CHECK(meter.status().counter_mwh == 0);
  char rec[1200];
  CHECK(meter.record_json(0, rec, sizeof(rec)));
  CHECK(json_is_null(rec, "interval_estimated_mwh"));
  char kind[32];
  CHECK(json_str(rec, "record_kind", kind, sizeof(kind)) && strcmp(kind, "gap") == 0);

  MemoryStore zstore;
  Meter zero;
  CHECK(open_meter(zero, zstore, test_cfg()));
  CHECK(zero.agree_anchor(utc, utc, 0, 2));
  AcSample z = sample_at(0, 230000, 0);
  CHECK(zero.on_sample(z));
  z.mono_ms = 1000;
  CHECK(zero.on_sample(z));
  CHECK(zero.status().counter_mwh == 0);
  CHECK(zero.flush(1000));
  CHECK(zero.record_json(0, rec, sizeof(rec)));
  uint64_t mwh = 1;
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 0);
  CHECK(json_str(rec, "attribution", kind, sizeof(kind)) && strcmp(kind, "valid_zero") == 0);
  CHECK(!json_is_null(rec, "interval_estimated_mwh"));

  MemoryStore istore;
  Meter invalid;
  CHECK(open_meter(invalid, istore, test_cfg()));
  CHECK(invalid.on_sample(sample_at(0, 230000, 1000)));
  AcSample bad = sample_at(1000, 230000, 1000);
  bad.valid = false;
  CHECK(invalid.on_sample(bad));
  CHECK(invalid.status().counter_mwh == 0);
  CHECK(invalid.record_json(0, rec, sizeof(rec)));
  CHECK(json_is_null(rec, "interval_estimated_mwh"));
  CHECK(json_str(rec, "unavailable_reason", kind, sizeof(kind)) && strcmp(kind, "sample_invalid") == 0);

  MemoryStore bstore;
  Meter bypass;
  CHECK(open_meter(bypass, bstore, test_cfg()));
  AcSample g = sample_at(0, 230000, 1000);
  g.mode = Mode::GridBypass;
  g.coverage = Coverage::InverterOutputOnly;
  CHECK(bypass.on_sample(g));
  g.mono_ms = 1000;
  CHECK(bypass.on_sample(g));
  CHECK(bypass.status().counter_mwh == 0);
  CHECK(bypass.record_json(0, rec, sizeof(rec)));
  CHECK(json_str(rec, "unavailable_reason", kind, sizeof(kind)) && strcmp(kind, "bypass_not_covered") == 0);

  MemoryStore cstore;
  Meter common;
  CHECK(open_meter(common, cstore, test_cfg()));
  AcSample c = sample_at(0, 1000, 3600000);
  c.mode = Mode::GridBypass;
  c.coverage = Coverage::CommonOutput;
  CHECK(common.on_sample(c));
  c.mono_ms = 1000;
  CHECK(common.on_sample(c));
  CHECK(common.status().counter_mwh == 800);

  AcSample missing = sample_at(5000, 1000, 3600000);
  missing.acquired = false;
  uint64_t before = common.status().counter_mwh;
  CHECK(!common.on_sample(missing));
  CHECK(common.status().counter_mwh == before);
}

static void test_reboot_fault_overflow_ack() {
  ModelConfig cfg = test_cfg();
  MemoryStore store;
  Meter meter;
  CHECK(open_meter(meter, store, cfg));
  CHECK(meter.on_sample(sample_at(0, 1000, 3600000)));
  CHECK(meter.on_sample(sample_at(1000, 1000, 3600000)));
  CHECK(meter.flush(1000));
  CHECK(meter.status().persisted_mwh == 800);
  CHECK(meter.on_sample(sample_at(1000, 1000, 3600000)));
  CHECK(meter.on_sample(sample_at(2000, 1000, 3600000)));
  CHECK(meter.status().counter_mwh == 1600);
  CHECK(meter.status().persisted_mwh == 800);
  MemoryStore crashed;
  store.clone_durable(crashed);
  Meter recovered;
  CHECK(recovered.begin(crashed, cfg));
  CHECK(recovered.status().persisted_mwh == 800);
  CHECK(recovered.status().counter_mwh == 800);
  CHECK(recovered.status().loss_events >= 1);
  CHECK(recovered.status().meter_epoch == 1);
  CHECK(recovered.status().pf_assumed_millis == 800);
  CHECK(recovered.on_sample(sample_at(3000, 1000, 3600000)));
  CHECK(recovered.on_sample(sample_at(4000, 1000, 3600000)));
  CHECK(recovered.status().counter_mwh == 1600);

  MemoryStore fstore;
  Meter fragile;
  CHECK(open_meter(fragile, fstore, cfg));
  CHECK(fragile.on_sample(sample_at(0, 1000, 3600000)));
  CHECK(fragile.on_sample(sample_at(1000, 1000, 3600000)));
  fstore.fail_next_persists(1);
  CHECK(!fragile.flush(1000));
  CHECK(fragile.status().persisted_mwh == 0);
  CHECK(fragile.status().counter_mwh == 800);
  CHECK(fragile.flush(1000));
  CHECK(fragile.status().persisted_mwh == 800);
  CHECK(fragile.status().committed_records == 1);

  ModelConfig tiny = cfg;
  tiny.slot_count = 2;
  tiny.max_integrate_ms = 2000;
  MemoryStore full;
  Meter box;
  CHECK(open_meter(box, full, tiny));
  uint64_t cursor = 0;
  for (int n = 0; n < 2; ++n) {
    CHECK(box.on_sample(sample_at(cursor, 1000, 3600000)));
    CHECK(box.on_sample(sample_at(cursor + 1000, 1000, 3600000)));
    CHECK(box.flush(cursor + 1000));
    cursor += 1000;
  }
  CHECK(box.status().committed_records == 2);
  CHECK(box.status().persisted_mwh == 1600);
  uint32_t loss_before = box.status().loss_events;
  CHECK(box.on_sample(sample_at(cursor, 1000, 3600000)));
  CHECK(box.on_sample(sample_at(cursor + 1000, 1000, 3600000)));
  CHECK(!box.flush(cursor + 1000));
  CHECK(box.status().overflow_alarm);
  CHECK(box.status().persisted_mwh == 1600);
  CHECK(box.status().committed_records == 2);
  char kept[1200];
  CHECK(box.record_json(0, kept, sizeof(kept)));
  CHECK(box.record_json(1, kept, sizeof(kept)));
  CHECK(!box.record_json(2, kept, sizeof(kept)));
  MemoryStore after;
  full.clone_durable(after);
  Meter boxed;
  CHECK(boxed.begin(after, tiny));
  CHECK(boxed.status().persisted_mwh == 1600);
  CHECK(boxed.status().loss_events > loss_before);

  CHECK(box.http_success_kept(box.status().boot_id, 1));
  CHECK(!box.note_http_success(box.status().boot_id, 1));
  CHECK(box.http_success_kept(box.status().boot_id, 1));
  uint64_t energy_before_ack = box.status().persisted_mwh;
  CHECK(box.note_server_commit(box.status().boot_id, 1));
  CHECK(box.note_server_commit(box.status().boot_id, 1));
  CHECK(!box.http_success_kept(box.status().boot_id, 1));
  CHECK(box.status().persisted_mwh == energy_before_ack);
  CHECK(box.record_json(0, kept, sizeof(kept)));
  uint64_t seq = 0;
  CHECK(json_u64(kept, "seq", &seq) && seq == 2);
}

static void test_time_midnight_and_replay_identity() {
  ModelConfig cfg = test_cfg();
  MemoryStore store;
  Meter meter;
  CHECK(open_meter(meter, store, cfg));
  CHECK(!meter.reject_stale_nvs(1700000000));
  CHECK(meter.status().time_quality == TimeQuality::StaleNvsRejected);
  CHECK(meter.on_sample(sample_at(0, 1000, 3600000)));
  CHECK(meter.on_sample(sample_at(1000, 1000, 3600000)));
  CHECK(meter.flush(1000));
  char rec[1200];
  CHECK(meter.record_json(0, rec, sizeof(rec)));
  CHECK(json_is_null(rec, "utc_start") && json_is_null(rec, "utc_end") && json_is_null(rec, "civil_day"));
  char attr[32];
  CHECK(json_str(rec, "attribution", attr, sizeof(attr)) && strcmp(attr, "unallocated") == 0);
  uint64_t mwh = 0;
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 800);
  int32_t kept_pf = 0;
  CHECK(json_i32(rec, "pf_assumed_millis", &kept_pf) && kept_pf == 800);
  char frozen[1200];
  memcpy(frozen, rec, sizeof(frozen));

  int64_t morning = unix_from_civil_utc(2026, 11, 8, 8, 0, 0);
  uint64_t boot_before = meter.status().boot_id;
  uint64_t epoch_before = meter.status().meter_epoch;
  uint64_t counter_before = meter.status().persisted_mwh;
  CHECK(meter.agree_anchor(morning, morning, 5000, 2));
  CHECK(meter.status().meter_epoch == epoch_before);
  CHECK(meter.status().boot_id == boot_before);
  CHECK(meter.status().persisted_mwh == counter_before);
  CHECK(meter.status().adjustments >= 1 || meter.status().time_quality == TimeQuality::Anchored);
  CHECK(meter.record_json(0, rec, sizeof(rec)));
  CHECK(strcmp(rec, frozen) == 0);

  uint8_t a[7], b[7];
  encode_ds1307(2026, 11, 8, 9, 0, 0, a);
  encode_ds1307(2026, 11, 8, 9, 0, 1, b);
  MemoryStore tstore;
  Meter tick;
  CHECK(open_meter(tick, tstore, cfg));
  CHECK(tick.observe_rtc(a, a, 0));
  CHECK(tick.observe_rtc(a, b, 1000));
  CHECK(tick.status().time_quality == TimeQuality::PlausibleTick);
  CHECK(!tick.status().estimate_available);
  CHECK(tick.on_sample(sample_at(1000, 1000, 3600000)));
  CHECK(tick.on_sample(sample_at(2000, 1000, 3600000)));
  CHECK(tick.flush(2000));
  CHECK(tick.record_json(0, rec, sizeof(rec)));
  CHECK(json_is_null(rec, "utc_start") && json_is_null(rec, "civil_day"));
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 800);

  int64_t midnight = unix_from_civil_utc(2024, 2, 29, 23, 0, 0);
  int64_t start = midnight - 10;
  MemoryStore nstore;
  Meter night;
  CHECK(open_meter(night, nstore, cfg));
  CHECK(night.agree_anchor(start, start, 100000, 1));
  AcSample s = sample_at(100000, 1000, 3600000);
  CHECK(night.on_sample(s));
  for (int k = 1; k <= 20; ++k) {
    s.mono_ms = 100000 + (uint64_t)k * 1000;
    CHECK(night.on_sample(s));
  }
  CHECK(night.flush(100000 + 20000));
  uint64_t day1 = 0, cross = 0, day2 = 0;
  int cross_n = 0, day1_n = 0, day2_n = 0;
  bool half = false;
  for (uint32_t i = 0; night.record_json(i, rec, sizeof(rec)); ++i) {
    char kind[32], day[16], attribution[32];
    CHECK(json_str(rec, "record_kind", kind, sizeof(kind)));
    uint64_t part = 0;
    bool has = json_u64(rec, "interval_estimated_mwh", &part);
    if (has && (part == 400 || part == 3600 || part == 4000)) half = true;
    if (strcmp(kind, "interval") == 0 && has) {
      CHECK(json_str(rec, "attribution", attribution, sizeof(attribution)));
      if (strcmp(attribution, "unallocated") == 0) { cross += part; cross_n++; }
      else if (!json_is_null(rec, "civil_day") && json_str(rec, "civil_day", day, sizeof(day))) {
        if (strcmp(day, "2024-02-29") == 0) { day1 += part; day1_n++; }
        if (strcmp(day, "2024-03-01") == 0) { day2 += part; day2_n++; }
      }
    }
    if (strcmp(kind, "daily_checkpoint") == 0) {
      CHECK(json_str(rec, "civil_day", day, sizeof(day)) && strcmp(day, "2024-02-29") == 0);
      uint64_t alloc = 0, unalloc = 0;
      CHECK(json_u64(rec, "allocated_estimated_mwh", &alloc));
      CHECK(json_u64(rec, "unallocated_estimated_mwh", &unalloc));
      CHECK(alloc == 7200);
      CHECK(unalloc == 800);
      CHECK(json_is_null(rec, "interval_estimated_mwh"));
    }
    CHECK(!json_has_ambiguous_energy_key(rec));
  }
  CHECK(!half);
  CHECK(day1 == 7200 && day1_n == 1);
  CHECK(cross == 800 && cross_n == 1);
  CHECK(day2 == 8000 && day2_n == 1);
  CHECK(night.status().counter_mwh == 16000);

  char status[2048];
  MeterStatus st = night.status();
  CHECK(status_json(st, "stm32_explicit_vrms_irms", true, status, sizeof(status)) > 0);
  CHECK(!json_has_ambiguous_energy_key(status));
  bool synthetic = true;
  CHECK(json_bool(status, "synthetic", &synthetic) && !synthetic);
  CHECK(json_str(status, "transport", attr, sizeof(attr)) && strcmp(attr, "pending_contract") == 0);
  CHECK(strstr(status, kMissingPath) != 0);

  uint8_t junk[256];
  memset(junk, 0xA5, sizeof(junk));
  CHECK(nstore.write(0, junk, sizeof(junk)));
  CHECK(nstore.persist());
  Meter broken;
  CHECK(broken.begin(nstore, cfg));
  CHECK(broken.status().journal_untrusted);
  CHECK(broken.status().persisted_mwh == 0);
  CHECK(broken.status().loss_events >= 1);
  CHECK(broken.status().meter_epoch == 1);
}

static void test_retained_pf_is_not_rewritten() {
  ModelConfig historical = test_cfg();
  historical.model_revision = 1;
  historical.pf_assumed_millis = 1000;
  MemoryStore store;
  Meter meter;
  CHECK(open_meter(meter, store, historical));
  CHECK(meter.status().pf_assumed_millis == 1000);
  CHECK(meter.status().model_revision == 1);
  CHECK(meter.on_sample(sample_at(0, 230000, 1000)));
  CHECK(meter.on_sample(sample_at(3600000, 230000, 1000)));
  CHECK(meter.flush(3600000));
  CHECK(meter.status().persisted_mwh == 230000);
  char original[1200];
  CHECK(meter.record_json(0, original, sizeof(original)));
  int32_t pf = 0;
  CHECK(json_i32(original, "pf_assumed_millis", &pf) && pf == 1000);
  char model[8];
  CHECK(json_str(original, "model_revision", model, sizeof(model)) && strcmp(model, "1") == 0);

  ModelConfig current = test_cfg();
  CHECK(current.pf_assumed_millis == 800 && current.model_revision == 2);
  MemoryStore kept;
  store.clone_durable(kept);
  Meter loaded;
  CHECK(loaded.begin(kept, current));
  CHECK(loaded.status().persisted_mwh == 230000);
  CHECK(loaded.status().pf_assumed_millis == 1000);
  CHECK(loaded.status().model_revision == 1);
  char after[1200];
  CHECK(loaded.record_json(0, after, sizeof(after)));
  CHECK(strcmp(after, original) == 0);
  CHECK(loaded.on_sample(sample_at(3600000, 1000, 3600000)));
  CHECK(loaded.on_sample(sample_at(3601000, 1000, 3600000)));
  CHECK(loaded.status().counter_mwh == 231000);
  CHECK(loaded.flush(3601000));
  CHECK(loaded.record_json(0, after, sizeof(after)));
  CHECK(strcmp(after, original) == 0);
  CHECK(loaded.record_json(1, after, sizeof(after)));
  CHECK(json_i32(after, "pf_assumed_millis", &pf) && pf == 1000);
  uint64_t added = 0;
  CHECK(json_u64(after, "interval_estimated_mwh", &added) && added == 1000);
}

int main() {
  test_crc_and_calendar();
  test_ds1307_and_backoff();
  test_arithmetic();
  test_source_qualification();
  test_meter_energy_and_journal();
  test_gaps_zero_bypass();
  test_reboot_fault_overflow_ack();
  test_time_midnight_and_replay_identity();
  test_retained_pf_is_not_rewritten();
  printf("load_ac_energy tests: pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
