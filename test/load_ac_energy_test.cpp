#include "load_ac_energy.h"
#include "load_ac_energy_upload.h"
#include "original_v01_profile.h"
#include "stm32_text_validity.h"

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
  char model_version[16], pf_kind[16];
  CHECK(json_str(rec, "version", model_version, sizeof(model_version)) && strcmp(model_version, "0.1.0") == 0);
  CHECK(json_str(rec, "pf_kind", pf_kind, sizeof(pf_kind)) && strcmp(pf_kind, "assumed") == 0);
  CHECK(json_i32(rec, "pf_assumption_millis", &pf) && pf == 800);
  char volts[16], amps[16];
  CHECK(json_str(rec, "ac_vrms_mv", volts, sizeof(volts)) && strcmp(volts, "230000") == 0);
  CHECK(json_str(rec, "ac_irms_ma", amps, sizeof(amps)) && strcmp(amps, "1000") == 0);
  printf("WIREJSON %s\n", rec);
  bool flag = false;
  bool simulation = true;
  char transport[64];
  (void)flag;
  (void)mv;
  (void)ma;
  CHECK(json_bool(rec, "simulation", &simulation) && !simulation);
  CHECK(json_str(rec, "stream", transport, sizeof(transport)) && strcmp(transport, "estimate.load_ac_energy") == 0);
  CHECK(json_str(rec, "contract", transport, sizeof(transport)) && strcmp(transport, kContract) == 0);
  CHECK(json_i32(rec, "pf_assumed_millis", &pf) && pf == 800);
  CHECK(json_bool(rec, "ac_v_valid", &simulation) && simulation);

  CHECK(!meter.set_pf_assumed(500, 3600000));
  CHECK(meter.set_pf_assumed(800, 3600000));
  CHECK(meter.status().counter_mwh == 184000);
  CHECK(meter.status().pf_assumed_millis == 800);

  MemoryStore reboot;
  store.clone_durable(reboot);
  Meter again;
  CHECK(again.begin(reboot, cfg));
  CHECK(again.status().boot_id == 2);
  CHECK(again.status().meter_epoch == 1);
  CHECK(again.status().persisted_mwh == 0);
  CHECK(again.status().pf_assumed_millis == 800);
  CHECK(again.status().next_seq == 1);
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
  uint64_t gap_mwh = 1;
  CHECK(json_u64(rec, "interval_estimated_mwh", &gap_mwh) && gap_mwh == 0);
  char kind[32];
  CHECK(json_str(rec, "sample_validity", kind, sizeof(kind)) && strcmp(kind, "absent") == 0);
  CHECK(json_str(rec, "coverage_scope", kind, sizeof(kind)) && strcmp(kind, "unknown") == 0);

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
  CHECK(json_str(rec, "sample_validity", kind, sizeof(kind)) && strcmp(kind, "valid") == 0);
  CHECK(json_str(rec, "ac_irms_ma", kind, sizeof(kind)) && strcmp(kind, "0") == 0);
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
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 0);
  CHECK(json_str(rec, "sample_validity", kind, sizeof(kind)) && strcmp(kind, "invalid") == 0);

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
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 0);
  CHECK(json_str(rec, "coverage_scope", kind, sizeof(kind)) && strcmp(kind, "unknown") == 0);

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
  char rec[1200];
  uint64_t mwh = 0;
  CHECK(recovered.status().persisted_mwh == 0);
  CHECK(recovered.status().counter_mwh == 0);
  CHECK(recovered.status().loss_events >= 1);
  CHECK(recovered.status().meter_epoch == 1);
  CHECK(recovered.status().pf_assumed_millis == 800);
  CHECK(recovered.record_json(0, rec, sizeof(rec)));
  CHECK(json_u64(rec, "interval_estimated_mwh", &mwh) && mwh == 800);
  CHECK(recovered.on_sample(sample_at(3000, 1000, 3600000)));
  CHECK(recovered.on_sample(sample_at(4000, 1000, 3600000)));
  CHECK(recovered.status().counter_mwh == 800);

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
  CHECK(boxed.status().persisted_mwh == 0);
  CHECK(boxed.record_json(0, kept, sizeof(kept)));
  CHECK(boxed.status().loss_events > loss_before);

  CHECK(box.record_json(0, kept, sizeof(kept)));
  CHECK(box.http_success_kept(box.status().boot_id, 1));
  CHECK(!box.note_http_success(box.status().boot_id, 1));
  CHECK(box.http_success_kept(box.status().boot_id, 1));
  CHECK(!box.note_server_commit(box.status().boot_id, 1));
  uint64_t energy_before_ack = box.status().persisted_mwh;
  char conflict[1200];
  snprintf(conflict, sizeof(conflict), "%s", kept);
  if (strlen(conflict) > 20) conflict[20] = conflict[20] == '1' ? '2' : '1';
  CHECK(!box.note_server_commit_replay(box.status().boot_id, 1, 0, conflict));
  CHECK(box.http_success_kept(box.status().boot_id, 1));
  CHECK(box.note_server_commit_replay(box.status().boot_id, 1, 0, kept));
  CHECK(box.note_server_commit_replay(box.status().boot_id, 1, 0, kept));
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
  CHECK(json_is_null(rec, "utc_start") && json_is_null(rec, "utc_end"));
  char attr[32];
  CHECK(json_str(rec, "time_quality", attr, sizeof(attr)) && strcmp(attr, "uncertain") == 0);
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
  CHECK(json_is_null(rec, "utc_start"));
  CHECK(json_str(rec, "time_quality", attr, sizeof(attr)) && strcmp(attr, "uncertain") == 0);
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
  uint64_t day1 = 0, day2 = 0;
  int day1_n = 0, day2_n = 0, straddles = 0;
  for (uint32_t i = 0; night.record_json(i, rec, sizeof(rec)); ++i) {
    char start_utc[40], end_utc[40], validity[16];
    uint64_t part = 0;
    CHECK(json_u64(rec, "interval_estimated_mwh", &part));
    CHECK(json_str(rec, "sample_validity", validity, sizeof(validity)));
    if (strcmp(validity, "valid") != 0) continue;
    CHECK(json_str(rec, "utc_start", start_utc, sizeof(start_utc)));
    CHECK(json_str(rec, "utc_end", end_utc, sizeof(end_utc)));
    bool starts_feb = strstr(start_utc, "2024-02-29T22:") != 0;
    bool ends_march = strstr(end_utc, "2024-02-29T23:") != 0 || strstr(end_utc, "2024-03-01T") != 0;
    if (starts_feb && ends_march && strstr(end_utc, "2024-02-29T23:00:00Z") == 0) straddles++;
    if (strstr(end_utc, "2024-02-29T23:00:00Z")) { day1 += part; day1_n++; }
    else if (strstr(start_utc, "2024-02-29T23:") || strstr(start_utc, "2024-03-01T")) { day2 += part; day2_n++; }
    CHECK(part != 400 && part != 4000);
  }
  CHECK(straddles == 0);
  CHECK(day1 == 8000 && day1_n == 1);
  CHECK(day2 == 8000 && day2_n >= 1);
  CHECK(night.status().counter_mwh == 16000);

  char status[2048];
  MeterStatus st = night.status();
  CHECK(status_json(st, "stm32_explicit_vrms_irms", true, status, sizeof(status)) > 0);
  printf("STATUSJSON %s\n", status);
  CHECK(json_str(status, "sample_validity", attr, sizeof(attr)) && strcmp(attr, "absent") == 0);
  bool simulation = true;
  CHECK(json_bool(status, "simulation", &simulation) && !simulation);
  CHECK(json_i32(status, "pf_assumption_millis", &kept_pf) && kept_pf == 800);
  CHECK(strstr(status, "\"inputs\":null") != 0);

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

static void test_text_validity_boundary() {
  Stm32TextAssembler rx;
  TextCandidate out;
  const char *frame[] = {
      "PC-CALEB-XFER4",
      "SYS : INVERTER",
      "VOUT: 230.1V",
      "IOUT: 1.25A",
      "SYS : INVERTER",
  };
  bool closed = false;
  for (int i = 0; i < 5; ++i) closed = rx.feed_line(1000 + (uint32_t)i * 10, frame[i], &out) || closed;
  CHECK(rx.banner_seen());
  CHECK(out.provisional_inverter_only);
  CHECK(!out.live_eligible);
  CHECK(out.v_mv == 230100 && out.i_ma == 1250);
  CHECK(!live_estimate_allowed(closed_lineage_gate(), out));
  LineageGate open_gate;
  open_gate.installed_lineage_verified = true;
  open_gate.rms_validity_transmitted = true;
  CHECK(live_estimate_allowed(open_gate, out));

  Stm32TextAssembler split;
  split.feed_line(0, "SYS : INVERTER", &out);
  split.feed_line(10, "VOUT: 230.0V", &out);
  CHECK(split.feed_line(20, "SYS : GRID", &out));
  CHECK(!out.provisional_inverter_only);
  CHECK(out.v_in_record && !out.i_in_record);
  split.feed_line(30, "IOUT: 1.25A", &out);
  CHECK(split.feed_line(40, "SYS : INVERTER", &out));
  CHECK(out.mode == TextMode::Grid);
  CHECK(!out.provisional_inverter_only);
  CHECK(strcmp(out.reason, "not_sys_inverter") == 0);

  Stm32TextAssembler bad;
  bad.feed_line(0, "SYS : INVERTER", &out);
  bad.feed_line(10, "VOUT: 12abc", &out);
  bad.feed_line(20, "IOUT: 1.25A", &out);
  CHECK(bad.feed_line(30, "SYS : INVERTER", &out));
  CHECK(!out.provisional_inverter_only);
  CHECK(strcmp(out.reason, "malformed_number") == 0);

  Stm32TextAssembler zero;
  zero.feed_line(0, "SYS : INVERTER", &out);
  zero.feed_line(10, "VOUT: 230V", &out);
  zero.feed_line(20, "IOUT: 0A", &out);
  CHECK(zero.feed_line(30, "SYS : INVERTER", &out));
  CHECK(!out.provisional_inverter_only);
  CHECK(strcmp(out.reason, "current_clamp_ambiguous") == 0);

  Stm32TextAssembler late;
  late.feed_line(0, "SYS : INVERTER", &out);
  late.feed_line(10, "VOUT: 230V", &out);
  late.feed_line(5000, "IOUT: 1.25A", &out);
  CHECK(late.feed_line(5010, "SYS : INVERTER", &out));
  CHECK(out.stale && !out.provisional_inverter_only);

  Stm32TextAssembler absent;
  absent.feed_line(0, "SYS : INVERTER", &out);
  CHECK(absent.feed_line(10, "SYS : INVERTER", &out));
  CHECK(!out.v_in_record && !out.i_in_record);
  CHECK(strcmp(out.reason, "split_record") == 0);

  TextCandidate locked;
  rx.note_mutex_failure(9, &locked);
  CHECK(locked.mutex_failed && !locked.provisional_inverter_only);
  CHECK(!locked.v_numeric && locked.v_mv == 0 && locked.i_ma == 0);
}

static void test_retained_pf_is_not_rewritten() {
  MemoryStore store;
  Meter meter;
  CHECK(open_meter(meter, store, test_cfg()));
  const char *legacy = "{\"interval_estimated_mwh\":\"230000\",\"pf_assumed_millis\":1000,\"model_revision\":\"1\"}";
  uint8_t slot[kDefaultSlotBytes];
  memset(slot, 0, sizeof(slot));
  uint32_t len = (uint32_t)strlen(legacy);
  slot[0] = 1;
  slot[2] = (uint8_t)len;
  slot[3] = (uint8_t)(len >> 8);
  slot[8] = 7;
  memcpy(slot + 20, legacy, len);
  uint32_t crc = crc32_ieee_bytes(slot + 8, 12 + len);
  slot[4] = (uint8_t)crc;
  slot[5] = (uint8_t)(crc >> 8);
  slot[6] = (uint8_t)(crc >> 16);
  slot[7] = (uint8_t)(crc >> 24);
  CHECK(store.write(kHeaderBytes, slot, kDefaultSlotBytes));
  CHECK(store.persist());
  MemoryStore kept;
  store.clone_durable(kept);
  Meter loaded;
  CHECK(loaded.begin(kept, test_cfg()));
  CHECK(loaded.status().persisted_mwh == 0);
  CHECK(loaded.status().pf_assumed_millis == 800);
  char after[kDefaultSlotBytes];
  CHECK(loaded.record_json(0, after, sizeof(after)));
  CHECK(strcmp(after, legacy) == 0);
  CHECK(strstr(after, "184000") == 0);
}

static bool emit_hour(Meter &meter, MemoryStore &store, char *rec, size_t n) {
  if (!open_meter(meter, store, test_cfg())) return false;
  if (!meter.on_sample(sample_at(0, 230000, 1000))) return false;
  if (!meter.on_sample(sample_at(3600000, 230000, 1000))) return false;
  if (!meter.flush(3600000)) return false;
  return meter.record_json(0, rec, (uint32_t)n);
}

static void plant_slot(MemoryStore &store, uint32_t index, uint64_t seq, uint32_t boot, const char *json) {
  uint8_t slot[kDefaultSlotBytes];
  memset(slot, 0, sizeof(slot));
  uint32_t len = (uint32_t)strlen(json);
  slot[0] = 1;
  slot[2] = (uint8_t)len;
  slot[3] = (uint8_t)(len >> 8);
  for (int i = 0; i < 8; ++i) slot[8 + i] = (uint8_t)(seq >> (8 * i));
  for (int i = 0; i < 4; ++i) slot[16 + i] = (uint8_t)(boot >> (8 * i));
  memcpy(slot + 20, json, len);
  uint32_t crc = crc32_ieee_bytes(slot + 8, 12 + len);
  slot[4] = (uint8_t)crc;
  slot[5] = (uint8_t)(crc >> 8);
  slot[6] = (uint8_t)(crc >> 16);
  slot[7] = (uint8_t)(crc >> 24);
  store.write(kHeaderBytes + index * kDefaultSlotBytes, slot, kDefaultSlotBytes);
  store.persist();
}

static bool ack_for(char *dst, size_t cap, const char *body, const char *replay, const char *record_id, const char *clock,
                    const char *simulation, const char *stream, const char *device) {
  char dev[40], epoch[24], boot[24], seq[24];
  if (!json_str(body, "device_id", dev, sizeof(dev))) return false;
  if (!json_str(body, "meter_epoch", epoch, sizeof(epoch))) return false;
  if (!json_str(body, "boot_id", boot, sizeof(boot))) return false;
  if (!json_str(body, "seq", seq, sizeof(seq))) return false;
  int wrote = snprintf(dst, cap,
                       "{\"status\":\"success\",\"ack\":true,\"replay\":%s,\"contract\":\"estimate.load_ac_energy.v1\","
                       "\"stream\":\"%s\",\"record_id\":%s,\"device_id\":\"%s\",\"meter_epoch\":\"%s\",\"boot_id\":\"%s\","
                       "\"seq\":\"%s\",\"clock_correction_version\":%s,\"simulation\":%s}",
                       replay, stream, record_id, device ? device : dev, epoch, boot, seq, clock, simulation);
  return wrote > 0 && (size_t)wrote < cap;
}

static bool take_frame(UploadSession &up, Meter &meter, uint32_t now, char *frame, size_t cap) {
  if (!up.arm(meter, now, frame, (uint32_t)cap)) return false;
  up.sent(now);
  return true;
}

static bool frame_wraps(const char *frame, const char *body) {
  const char *prefix = "{\"type\":\"energy_estimate\",\"record\":";
  size_t plen = strlen(prefix);
  size_t blen = strlen(body);
  if (strncmp(frame, prefix, plen) != 0) return false;
  if (memcmp(frame + plen, body, blen) != 0) return false;
  return frame[plen + blen] == '}' && frame[plen + blen + 1] == 0;
}

static void test_socket_ack_184000() {
  MemoryStore store;
  Meter meter;
  char body[kDefaultSlotBytes];
  CHECK(emit_hour(meter, store, body, sizeof(body)));
  uint64_t mwh = 0;
  int32_t pf = 0;
  bool simulation = true;
  CHECK(json_u64(body, "interval_estimated_mwh", &mwh) && mwh == 184000);
  CHECK(json_i32(body, "pf_assumed_millis", &pf) && pf == 800);
  CHECK(json_bool(body, "simulation", &simulation) && !simulation);
  CHECK(!meter.note_http_success(meter.status().boot_id, 1));
  CHECK(!meter.note_server_commit(meter.status().boot_id, 1));
  CHECK(meter.http_success_kept(meter.status().boot_id, 1));

  const char *legacy = "{\"interval_estimated_mwh\":\"230000\",\"pf_assumed_millis\":1000,\"model_revision\":\"1\"}";
  plant_slot(store, 1, 0, 1, legacy);
  char legacy_now[200];
  CHECK(meter.record_json(0, legacy_now, sizeof(legacy_now)));
  CHECK(strcmp(legacy_now, legacy) == 0);

  UploadSession up;
  up.on_link(true, 1000);
  char frame[2048];
  CHECK(take_frame(up, meter, 1000, frame, sizeof(frame)));
  CHECK(frame_wraps(frame, body));
  CHECK(strstr(frame, "\"api_key\"") == 0);
  CHECK(strstr(frame, "device_secret") == 0);
  printf("UPLOADFRAME %s\n", frame);
  char saved_frame[2048];
  memcpy(saved_frame, frame, sizeof(saved_frame));

  char ack[1024];
  char dev[40], epoch[24], boot[24], seq[24];
  CHECK(json_str(body, "device_id", dev, sizeof(dev)));
  CHECK(json_str(body, "meter_epoch", epoch, sizeof(epoch)));
  CHECK(json_str(body, "boot_id", boot, sizeof(boot)));
  CHECK(json_str(body, "seq", seq, sizeof(seq)));
  int spaced = snprintf(ack, sizeof(ack),
                        "{ \"status\" : \"success\" , \"ack\" : true , \"replay\" : false , "
                        "\"contract\" : \"estimate.load_ac_energy.v1\" , \"stream\" : \"estimate.load_ac_energy\" , "
                        "\"record_id\" : 29 , \"device_id\" : \"%s\" , \"meter_epoch\" : \"%s\" , "
                        "\"boot_id\" : \"%s\" , \"seq\" : \"%s\" , \"clock_correction_version\" : 0 , "
                        "\"simulation\" : false }",
                        dev, epoch, boot, seq);
  CHECK(spaced > 0);
  up.on_text(meter, "{\"status\":\"success\"}", 1000);
  CHECK(up.inflight());
  CHECK(meter.http_success_kept(meter.status().boot_id, 1));
  up.on_text(meter, "{", 1100);
  CHECK(up.inflight());
  CHECK(strcmp(up.diagnostic(), "malformed") == 0);
  up.on_text(meter, ack, 1200);
  CHECK(!up.inflight());
  CHECK(strcmp(up.diagnostic(), "retired") == 0);
  CHECK(up.last_record_id() == 29);
  CHECK(!meter.http_success_kept(meter.status().boot_id, 1));
  CHECK(meter.status().persisted_mwh == 184000);
  CHECK(meter.status().pf_assumed_millis == 800);
  char still_legacy[200];
  CHECK(meter.record_json(0, still_legacy, sizeof(still_legacy)));
  CHECK(strcmp(still_legacy, legacy) == 0);
  CHECK(!meter.record_json(1, still_legacy, sizeof(still_legacy)));
  uint8_t retired[kDefaultSlotBytes];
  CHECK(store.read(kHeaderBytes, retired, sizeof(retired)));
  CHECK(retired[0] == 2);
  uint32_t rlen = (uint32_t)retired[2] | ((uint32_t)retired[3] << 8);
  CHECK(rlen == strlen(body));
  CHECK(memcmp(retired + 20, body, rlen) == 0);
  up.on_text(meter, ack, 1300);
  CHECK(strcmp(up.diagnostic(), "retired") == 0);
  uint8_t retired_again[kDefaultSlotBytes];
  CHECK(store.read(kHeaderBytes, retired_again, sizeof(retired_again)));
  CHECK(memcmp(retired_again, retired, sizeof(retired)) == 0);

  MemoryStore replay_store;
  Meter replay_meter;
  char replay_body[kDefaultSlotBytes];
  CHECK(emit_hour(replay_meter, replay_store, replay_body, sizeof(replay_body)));
  UploadSession replay;
  replay.on_link(true, 0);
  CHECK(take_frame(replay, replay_meter, 0, frame, sizeof(frame)));
  CHECK(frame_wraps(frame, replay_body));
  replay.on_link(false, 50);
  CHECK(strcmp(replay.diagnostic(), "disconnected") == 0);
  CHECK(!replay.inflight());
  CHECK(replay_meter.http_success_kept(replay_meter.status().boot_id, 1));
  replay.on_link(true, 50);
  char retry[2048];
  CHECK(take_frame(replay, replay_meter, 50, retry, sizeof(retry)));
  CHECK(strcmp(retry, frame) == 0);
  CHECK(ack_for(ack, sizeof(ack), replay_body, "true", "29", "0", "false", kStream, 0));
  replay.on_text(replay_meter, ack, 60);
  CHECK(strcmp(replay.diagnostic(), "retired") == 0);
  CHECK(replay.last_record_id() == 29);
  CHECK(!replay_meter.http_success_kept(replay_meter.status().boot_id, 1));
  CHECK(replay_meter.status().persisted_mwh == 184000);
}

static void relink(UploadSession &up, uint32_t now) {
  up.reset(now);
  up.on_link(true, now);
}

static void test_socket_ack_rejections_keep_bytes() {
  MemoryStore store;
  Meter meter;
  char body[kDefaultSlotBytes];
  CHECK(emit_hour(meter, store, body, sizeof(body)));
  char frozen[kDefaultSlotBytes];
  memcpy(frozen, body, sizeof(frozen));
  UploadSession up;
  up.on_link(true, 0);
  char frame[2048];
  char first_frame[2048];
  CHECK(take_frame(up, meter, 0, frame, sizeof(frame)));
  memcpy(first_frame, frame, sizeof(first_frame));
  char dev[40], epoch[24], boot[24], seq[24];
  CHECK(json_str(body, "device_id", dev, sizeof(dev)));
  CHECK(json_str(body, "meter_epoch", epoch, sizeof(epoch)));
  CHECK(json_str(body, "boot_id", boot, sizeof(boot)));
  CHECK(json_str(body, "seq", seq, sizeof(seq)));

  char bad[1400];
  const char *cases[] = {
      "{\"status\":\"success\",\"ack\":true,\"replay\":false,\"contract\":\"estimate.load_ac_energy.v1\","
      "\"stream\":\"estimate.load_ac_energy\",\"record_id\":\"29\",\"device_id\":\"%s\",\"meter_epoch\":\"%s\","
      "\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":0,\"simulation\":false}",
      "{\"status\":\"success\",\"ack\":true,\"replay\":false,\"contract\":\"estimate.load_ac_energy.v1\","
      "\"stream\":\"estimate.load_ac_energy\",\"record_id\":0,\"device_id\":\"%s\",\"meter_epoch\":\"%s\","
      "\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":0,\"simulation\":false}",
      "{\"status\":\"success\",\"ack\":true,\"replay\":false,\"contract\":\"estimate.load_ac_energy.v1\","
      "\"stream\":\"measured.load_ac_energy\",\"record_id\":29,\"device_id\":\"%s\",\"meter_epoch\":\"%s\","
      "\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":0,\"simulation\":false}",
      "{\"status\":\"success\",\"ack\":true,\"replay\":false,\"contract\":\"estimate.load_ac_energy.v1\","
      "\"stream\":\"estimate.load_ac_energy\",\"record_id\":true,\"device_id\":\"%s\",\"meter_epoch\":\"%s\","
      "\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":0,\"simulation\":false}",
      "{\"status\":\"success\",\"ack\":true,\"replay\":false,\"contract\":\"estimate.load_ac_energy.v1\","
      "\"stream\":\"estimate.load_ac_energy\",\"record_id\":29,\"device_id\":\"%s\",\"meter_epoch\":\"%s\","
      "\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":true,\"simulation\":false}",
      "{\"status\":\"success\",\"ack\":true,\"replay\":false,\"contract\":\"estimate.load_ac_energy.v1\","
      "\"stream\":\"estimate.load_ac_energy\",\"record_id\":29,\"device_id\":\"%s\",\"meter_epoch\":\"%s\","
      "\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":0,\"simulation\":false,\"api_key\":\"no\"}",
  };
  for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    relink(up, 1000u * (i + 1));
    CHECK(take_frame(up, meter, 1000u * (i + 1), frame, sizeof(frame)));
    CHECK(strcmp(frame, first_frame) == 0);
    snprintf(bad, sizeof(bad), cases[i], dev, epoch, boot, seq);
    up.on_text(meter, bad, 1000u * (i + 1) + 1);
    CHECK(strcmp(up.diagnostic(), "not_durable") == 0);
    CHECK(meter.http_success_kept(1, 1));
    CHECK(meter.record_json(0, body, sizeof(body)));
    CHECK(strcmp(body, frozen) == 0);
  }

  relink(up, 20000);
  CHECK(take_frame(up, meter, 20000, frame, sizeof(frame)));
  CHECK(ack_for(bad, sizeof(bad), frozen, "false", "29", "1", "false", kStream, 0));
  up.on_text(meter, bad, 20001);
  CHECK(strcmp(up.diagnostic(), "identity_mismatch") == 0);
  CHECK(meter.http_success_kept(1, 1));
  relink(up, 21000);
  CHECK(take_frame(up, meter, 21000, frame, sizeof(frame)));
  CHECK(ack_for(bad, sizeof(bad), frozen, "false", "29", "0", "true", kStream, 0));
  up.on_text(meter, bad, 21001);
  CHECK(strcmp(up.diagnostic(), "identity_mismatch") == 0);
  CHECK(meter.record_json(0, body, sizeof(body)) && strcmp(body, frozen) == 0);

  relink(up, 22000);
  CHECK(take_frame(up, meter, 22000, frame, sizeof(frame)));
  up.on_text(meter, "{\"status\":\"success\"}", 22001);
  CHECK(up.inflight());
  CHECK(meter.http_success_kept(1, 1));
  snprintf(bad, sizeof(bad),
           "{\"status\":\"error\",\"ack\":false,\"error\":\"conflicting_duplicate\","
           "\"message\":\"The same identity was already stored with a different record.\","
           "\"contract\":\"estimate.load_ac_energy.v1\",\"stream\":\"estimate.load_ac_energy\","
           "\"device_id\":\"%s\",\"meter_epoch\":\"%s\",\"boot_id\":\"%s\",\"seq\":\"%s\",\"clock_correction_version\":0}",
           dev, epoch, boot, seq);
  up.on_text(meter, bad, 22002);
  CHECK(strcmp(up.diagnostic(), "conflicting_duplicate") == 0);
  CHECK(!up.inflight());
  CHECK(!take_frame(up, meter, 22002 + kUploadRetryMinMs - 1, frame, sizeof(frame)));
  CHECK(take_frame(up, meter, 22002 + kUploadRetryMinMs, frame, sizeof(frame)));
  CHECK(strcmp(frame, first_frame) == 0);
  int32_t pf = 0;
  uint64_t mwh = 0;
  CHECK(meter.record_json(0, body, sizeof(body)) && strcmp(body, frozen) == 0);
  CHECK(json_u64(body, "interval_estimated_mwh", &mwh) && mwh == 184000);
  CHECK(json_i32(body, "pf_assumed_millis", &pf) && pf == 800);
  CHECK(!meter.set_pf_assumed(500, 3600000));

  snprintf(bad, sizeof(bad),
           "{\"status\":\"error\",\"ack\":false,\"error\":\"model_pf\",\"message\":\"estimate.load_ac_energy.v1 assumes PF 0.800.\","
           "\"contract\":\"estimate.load_ac_energy.v1\",\"stream\":\"estimate.load_ac_energy\"}");
  up.on_text(meter, bad, 23000 + kUploadRetryMinMs);
  CHECK(strcmp(up.diagnostic(), "model_pf") == 0);
  CHECK(meter.record_json(0, body, sizeof(body)) && strcmp(body, frozen) == 0);

  relink(up, 40000);
  CHECK(take_frame(up, meter, 40000, frame, sizeof(frame)));
  snprintf(bad, sizeof(bad),
           "{\"status\":\"error\",\"ack\":false,\"error\":\"unavailable\",\"message\":\"Estimate was not saved.\","
           "\"contract\":\"estimate.load_ac_energy.v1\",\"stream\":\"estimate.load_ac_energy\"}");
  up.on_text(meter, bad, 40001);
  CHECK(strcmp(up.diagnostic(), "unavailable") == 0);
  CHECK(meter.http_success_kept(1, 1));
  CHECK(strcmp(frame, first_frame) == 0);

  relink(up, 41000);
  CHECK(take_frame(up, meter, 41000, frame, sizeof(frame)));
  snprintf(bad, sizeof(bad),
           "{\"status\":\"error\",\"ack\":false,\"error\":\"device_mismatch\","
           "\"message\":\"device_id does not match the authenticated device.\","
           "\"contract\":\"estimate.load_ac_energy.v1\",\"stream\":\"estimate.load_ac_energy\","
           "\"device_id\":\"%s\",\"seq\":\"9\"}",
           dev);
  up.on_text(meter, bad, 41001);
  CHECK(up.inflight());
  CHECK(meter.http_success_kept(1, 1));
  up.on_text(meter, "{\"status\":\"success\",\"ack\":1}", 41002);
  CHECK(meter.http_success_kept(1, 1));
  CHECK(!up.arm(meter, 41000 + kAckWaitMs - 1, frame, sizeof(frame)));
  CHECK(up.inflight());
  CHECK(!up.arm(meter, 41000 + kAckWaitMs, frame, sizeof(frame)));
  CHECK(!up.inflight());
  CHECK(strcmp(up.diagnostic(), "ack_timeout") == 0);
  CHECK(meter.record_json(0, body, sizeof(body)) && strcmp(body, frozen) == 0);

  up.reset(42000);
  CHECK(strcmp(up.diagnostic(), "reset") == 0);
  CHECK(!up.inflight());
  CHECK(meter.record_json(0, body, sizeof(body)) && strcmp(body, frozen) == 0);
  CHECK(meter.status().persisted_mwh == 184000);
  CHECK(!meter.status().transport_enabled);
}

static void test_socket_ack_persist_and_queue() {
  MemoryStore store;
  Meter meter;
  char first[kDefaultSlotBytes];
  CHECK(emit_hour(meter, store, first, sizeof(first)));
  uint8_t torn = 2;
  CHECK(store.write(kHeaderBytes, &torn, 1));
  MemoryStore durable;
  store.clone_durable(durable);
  Meter rebooted;
  CHECK(rebooted.begin(durable, test_cfg()));
  char after_power[kDefaultSlotBytes];
  CHECK(rebooted.record_json(0, after_power, sizeof(after_power)));
  CHECK(strcmp(after_power, first) == 0);
  CHECK(rebooted.status().pf_assumed_millis == 800);

  MemoryStore cut;
  Meter cutting;
  char body[kDefaultSlotBytes];
  CHECK(emit_hour(cutting, cut, body, sizeof(body)));
  UploadSession up;
  up.on_link(true, 0);
  char frame[2048];
  CHECK(take_frame(up, cutting, 0, frame, sizeof(frame)));
  char ack[1024];
  CHECK(ack_for(ack, sizeof(ack), body, "false", "41", "0", "false", kStream, 0));
  cut.fail_next_persists(1);
  up.on_text(cutting, ack, 5);
  CHECK(strcmp(up.diagnostic(), "retire_not_durable") == 0);
  CHECK(cutting.http_success_kept(1, 1));
  char kept[kDefaultSlotBytes];
  CHECK(cutting.record_json(0, kept, sizeof(kept)));
  CHECK(strcmp(kept, body) == 0);
  CHECK(take_frame(up, cutting, 5 + kUploadRetryMinMs, frame, sizeof(frame)));
  CHECK(frame_wraps(frame, body));
  up.on_text(cutting, ack, 6 + kUploadRetryMinMs);
  CHECK(strcmp(up.diagnostic(), "retired") == 0);
  CHECK(up.last_record_id() == 41);
  CHECK(!cutting.http_success_kept(1, 1));

  MemoryStore queued;
  Meter queue;
  CHECK(open_meter(queue, queued, test_cfg()));
  CHECK(queue.on_sample(sample_at(0, 230000, 1000)));
  CHECK(queue.on_sample(sample_at(3600000, 230000, 1000)));
  CHECK(queue.flush(3600000));
  CHECK(queue.on_sample(sample_at(3600000, 230000, 1000)));
  CHECK(queue.on_sample(sample_at(7200000, 230000, 1000)));
  CHECK(queue.flush(7200000));
  char a[kDefaultSlotBytes], b[kDefaultSlotBytes], b_frozen[kDefaultSlotBytes];
  CHECK(queue.record_json(0, a, sizeof(a)));
  CHECK(queue.record_json(1, b, sizeof(b)));
  memcpy(b_frozen, b, sizeof(b_frozen));
  uint64_t seq = 0;
  CHECK(json_u64(a, "seq", &seq) && seq == 1);
  CHECK(json_u64(b, "seq", &seq) && seq == 2);
  UploadSession qu;
  qu.on_link(true, 0);
  CHECK(take_frame(qu, queue, 0, frame, sizeof(frame)));
  CHECK(frame_wraps(frame, a));
  CHECK(ack_for(ack, sizeof(ack), b, "false", "8", "0", "false", kStream, 0));
  qu.on_text(queue, ack, 1);
  CHECK(strcmp(qu.diagnostic(), "identity_mismatch") == 0);
  CHECK(queue.record_json(0, a, sizeof(a)));
  CHECK(queue.record_json(1, b, sizeof(b)));
  CHECK(strcmp(b, b_frozen) == 0);
  CHECK(ack_for(ack, sizeof(ack), a, "false", "7", "0", "false", kStream, 0));
  CHECK(take_frame(qu, queue, 1 + kUploadRetryMinMs, frame, sizeof(frame)));
  qu.on_text(queue, ack, 2 + kUploadRetryMinMs);
  CHECK(strcmp(qu.diagnostic(), "retired") == 0);
  CHECK(queue.record_json(0, b, sizeof(b)));
  CHECK(strcmp(b, b_frozen) == 0);
  CHECK(!queue.record_json(1, b, sizeof(b)));

  UploadSession idle;
  CHECK(idle.arm(queue, 0, frame, sizeof(frame)) == false);
  idle.on_link(true, 3 + kUploadRetryMinMs);
  CHECK(take_frame(idle, queue, 3 + kUploadRetryMinMs, frame, sizeof(frame)));
  idle.send_failed(4 + kUploadRetryMinMs);
  CHECK(strcmp(idle.diagnostic(), "send_failed") == 0);
  CHECK(queue.record_json(0, b, sizeof(b)));
  CHECK(strcmp(b, b_frozen) == 0);

  const char *versioned = "{\"clock_correction_version\":10,\"kept\":1}";
  MemoryStore versions;
  Meter version_meter;
  CHECK(open_meter(version_meter, versions, test_cfg()));
  plant_slot(versions, 0, 9, 3, versioned);
  CHECK(!version_meter.note_server_commit_replay(3, 9, 1, versioned));
  CHECK(version_meter.http_success_kept(3, 9));
  CHECK(version_meter.note_server_commit_replay(3, 9, 10, versioned));
  CHECK(!version_meter.http_success_kept(3, 9));
  uint8_t slot[kDefaultSlotBytes];
  CHECK(versions.read(kHeaderBytes, slot, sizeof(slot)));
  CHECK(slot[0] == 2);
  CHECK(memcmp(slot + 20, versioned, strlen(versioned)) == 0);
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
  test_text_validity_boundary();
  test_retained_pf_is_not_rewritten();
  test_socket_ack_184000();
  test_socket_ack_rejections_keep_bytes();
  test_socket_ack_persist_and_queue();
  printf("load_ac_energy tests: pass=%d fail=%d\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
