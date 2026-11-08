#pragma once
#include <stddef.h>
#include <stdint.h>

// estimate.load_ac_energy.v1
//
// Estimated load energy from independently acquired AC Vrms and Irms:
//   estimated_mW = Vrms_mV * Irms_mA * PF_millis / 1_000_000
//   estimated_mWh accumulates that power over a known dt, integer division,
//   remainder kept. PF_millis is PF * 1000 and is an assumption, never a
//   measured power factor. The original v0.1 default is 800 (PF = 0.800),
//   chosen by Caleb on 2026-11-08. It is not an accuracy claim.
//   model_revision 2 is that assumption. A stored record keeps the
//   model_revision and pf_assumed_millis it was written with; loading a
//   journal does not rewrite those bytes or rescale its counter.
//
// BMS pack discharge, the ADS1115 PV bus, unlabeled STM32 VOUT/IOUT, and
// v0.1.1 PZEM watt-hours are not inputs. PZEM watt-hours are not multiplied
// by 0.8. Unknown gaps and uncovered bypass intervals are not integrated.

namespace loadac {

constexpr const char *kContract = "estimate.load_ac_energy.v1";
constexpr const char *kStream = "estimate.load_ac_energy";
constexpr int32_t kPfAssumedMillis = 800;
constexpr const char *kModelWireVersion = "0.1.0";
constexpr const char *kModelSourcePath = "ac_vrms_mv_x_ac_irms_ma_x_pf_millis";
constexpr uint32_t kModelRevision = 2;
constexpr uint64_t kEnergyDenom = 3600000000000ULL;
constexpr uint32_t kHeaderBytes = 256;
constexpr uint32_t kDefaultSlots = 32;
constexpr uint32_t kDefaultSlotBytes = 1280;
constexpr uint32_t kStoreBytes = kHeaderBytes + kDefaultSlots * kDefaultSlotBytes;
constexpr int64_t kLagosOffsetSeconds = 3600;

// Why the pinned original v0.1 firmware cannot feed the estimator today.
constexpr const char *kMissingPath =
    "pinned main a0932ad src/inverter.cpp UART1 RX33 TX2 9600 text parses VOUT and IOUT only; "
    "finalizeRecord stores 0 after three missed records; no Vrms, Irms, validity, or CT coverage. "
    "Provenance 55b18aa calls those fields inverter output volts and CT amps and does not integrate V*I; "
    "RAW_RMS is not decoded there and is absent here. Framed vout_dV/iout_cA/VALID_OUTPUT is not in this tree. "
    "src/energy.cpp energy_update is JK BMS DC discharge. src/mppt.cpp ADS1115 is the DC PV bus. "
    "No PZEM is fitted. Bypass coverage of the CT is not established.";

enum class Mode : uint8_t { Unknown = 0, Idle, Inverter, GridBypass, Transfer, Fault };
enum class Coverage : uint8_t { Unknown = 0, InverterOutputOnly, CommonOutput };
enum class TimeQuality : uint8_t {
  Uncertain = 0,
  Unanchored,
  PlausibleTick,
  Anchored,
  Adjusted,
  StaleNvsRejected
};
enum class Attribution : uint8_t { Unallocated = 0, Allocated, ValidZero };
enum class RecordKind : uint8_t { Status = 0, Interval, Gap, DailyCheckpoint };
enum class UnavailableReason : uint8_t {
  None = 0,
  SourceUnavailable,
  Stm32TextVoutIoutNotRms,
  MissForcedZero,
  RmsValidityFalse,
  CoverageUnknown,
  ModeUnknown,
  BypassNotCovered,
  BmsDcDischarge,
  AdsDcPvBus,
  PfNotAssumption,
  SampleMissing,
  SampleInvalid
};

struct ModelConfig {
  uint32_t model_revision;
  uint32_t source_version;
  int32_t pf_assumed_millis;
  uint32_t max_integrate_ms;
  uint32_t commit_interval_ms;
  uint32_t wear_window_ms;
  uint32_t max_commits_per_window;
  uint16_t slot_count;
  uint16_t slot_bytes;
};

struct AcSample {
  uint64_t mono_ms;
  bool acquired;
  bool valid;
  int32_t v_rms_mV;
  int32_t i_rms_mA;
  Mode mode;
  Coverage coverage;
  uint32_t source_version;
  const char *source_path;
};

struct Stm32Observation {
  bool has_vout;
  bool has_iout;
  bool has_vrms;
  bool has_irms;
  bool vrms_valid;
  bool irms_valid;
  bool miss_forced_zero;
  int32_t vout_mV;
  int32_t iout_mA;
  int32_t vrms_mV;
  int32_t irms_mA;
  bool has_mode;
  bool has_coverage;
  Mode mode;
  Coverage coverage;
  uint64_t mono_ms;
  uint32_t source_version;
  const char *source_path;
};

struct CivilDay {
  int year;
  int month;
  int day;
  bool valid;
};

struct RtcReading {
  bool ok;
  bool halted;
  int64_t unix_utc;
  CivilDay face;
};

struct I2cBackoff {
  uint32_t suppress_until_ms;
  uint32_t backoff_ms;
  uint32_t failures;
};

class ByteStore {
 public:
  virtual ~ByteStore() {}
  virtual bool read(uint32_t offset, uint8_t *dst, uint32_t n) = 0;
  virtual bool write(uint32_t offset, const uint8_t *src, uint32_t n) = 0;
  virtual uint32_t size() const = 0;
  // Make every write since the previous successful persist durable.
  // On failure the store restores the last durable image.
  virtual bool persist() = 0;
};

class MemoryStore : public ByteStore {
 public:
  MemoryStore();
  bool read(uint32_t offset, uint8_t *dst, uint32_t n) override;
  bool write(uint32_t offset, const uint8_t *src, uint32_t n) override;
  uint32_t size() const override { return kStoreBytes; }
  bool persist() override;
  void fail_next_persists(int count) { fail_persists_ = count; }
  void clone_durable(MemoryStore &dst) const;
  bool durable_equals(const MemoryStore &o) const;

 private:
  uint8_t live_[kStoreBytes];
  uint8_t durable_[kStoreBytes];
  int fail_persists_;
};

ModelConfig default_model();
const char *reason_name(UnavailableReason r);
const char *mode_name(Mode m);
const char *coverage_name(Coverage c);
const char *quality_name(TimeQuality q);
const char *attribution_name(Attribution a);
const char *kind_name(RecordKind k);

bool coverage_allows(Mode mode, Coverage coverage);
AcSample qualify_stm32(const Stm32Observation &in, UnavailableReason *why);
AcSample reject_bms_dc(uint64_t mono_ms);
AcSample reject_ads_pv(uint64_t mono_ms);
Stm32Observation pinned_main_stm32_observation(uint64_t mono_ms);
bool ds1307_has_osf_bit();

bool mul_u64(uint64_t a, uint64_t b, uint64_t *out);
bool estimated_mwh(int32_t v_mV, int32_t i_mA, int32_t pf_millis, uint64_t dt_ms,
                   uint64_t *residue, uint64_t *added_mwh);

bool lagos_day(int64_t unix_utc, CivilDay *out);
bool same_lagos_day(int64_t a, int64_t b);
bool is_leap_year(int year);
bool civil_valid(int year, int month, int day);
int64_t unix_from_civil_utc(int year, int month, int day, int hour, int minute, int second);

RtcReading decode_ds1307(const uint8_t reg[7]);
bool ds1307_pair(const uint8_t first[7], const uint8_t second[7], RtcReading *out);
void encode_ds1307(int year, int month, int day, int hour, int minute, int second, uint8_t reg[7]);

bool i2c_allow(const I2cBackoff &b, uint32_t now_ms);
void i2c_fail(I2cBackoff *b, uint32_t now_ms);
void i2c_ok(I2cBackoff *b);

bool json_has_ambiguous_energy_key(const char *json);

struct MeterStatus {
  bool estimate_available;
  bool counter_trusted;
  bool journal_untrusted;
  bool overflow_alarm;
  bool transport_enabled;
  bool synthetic;
  bool tail_open;
  bool durable;
  UnavailableReason reason;
  TimeQuality time_quality;
  Attribution attribution;
  bool interval_present;
  uint64_t interval_mwh;
  bool allocated_present;
  uint64_t allocated_report_mwh;
  uint64_t counter_mwh;
  uint64_t persisted_mwh;
  uint64_t residue;
  uint64_t meter_epoch;
  uint32_t boot_id;
  uint64_t next_seq;
  uint32_t loss_events;
  uint32_t adjustments;
  uint64_t allocated_mwh;
  uint64_t unallocated_mwh;
  CivilDay civil_day;
  int32_t pf_assumed_millis;
  uint32_t model_revision;
  uint32_t source_version;
  uint32_t pending_ram;
  uint32_t committed_records;
  char device_id[16];
  char civil_text[16];
};

uint32_t status_json(const MeterStatus &status, const char *source_path, bool include_missing_path,
                      char *dst, uint32_t cap);
uint32_t crc32_ieee_bytes(const uint8_t *data, uint32_t n);

class Meter {
 public:
  Meter();
  bool begin(ByteStore &store, const ModelConfig &cfg);
  bool set_device_id(const char *id);
  void set_source_blocked(UnavailableReason why);

  bool on_sample(const AcSample &sample);
  bool flush(uint64_t mono_ms);
  bool set_pf_assumed(int32_t pf_millis, uint64_t mono_ms);
  bool observe_rtc(const uint8_t first[7], const uint8_t second[7], uint64_t mono_ms);
  bool agree_anchor(int64_t rtc_unix, int64_t external_unix, uint64_t mono_ms, int64_t tolerance_s);
  bool reject_stale_nvs(int64_t saved_unix);
  bool poll_day(uint64_t mono_ms);

  bool note_http_success(uint32_t boot_id, uint64_t seq);
  bool note_server_commit(uint32_t boot_id, uint64_t seq);
  // Ack only when the committed body is the stored body. Identity is
  // device_id + stream + meter_epoch + boot_id + seq + clock_correction_version.
  // A different body for that identity is a conflict and is not stored.
  bool note_server_commit_replay(uint32_t boot_id, uint64_t seq, uint32_t clock_correction_version, const char *body);

  MeterStatus status() const;
  uint32_t copy_records(char *dst, uint32_t dst_bytes, uint32_t *count) const;
  bool record_json(uint32_t ordinal, char *dst, uint32_t dst_bytes) const;
  bool http_success_kept(uint32_t boot_id, uint64_t seq) const;

 private:
  struct Segment {
    bool open;
    bool integrable;
    bool valid_zero;
    bool crossing;
    RecordKind kind;
    Attribution attribution;
    uint64_t mono_start;
    uint64_t mono_end;
    uint64_t counter_start;
    uint64_t added_mwh;
    uint32_t covered_ms;
    uint64_t anchor_gen_start;
    int64_t utc_start;
    bool utc_start_known;
    int32_t v_mV;
    int32_t i_mA;
    int32_t pf_millis;
    Mode mode;
    Coverage coverage;
    uint32_t source_version;
    char source_path[48];
    UnavailableReason reason;
  };

  struct Clock {
    TimeQuality quality;
    bool anchored;
    uint64_t generation;
    int64_t utc_anchor;
    uint64_t mono_anchor;
    bool have_face;
    int64_t last_unix;
    uint64_t last_mono;
    uint32_t adjustments;
  };

  bool save_header(bool persist_now);
  bool persist_image();
  bool commit_segment(uint64_t mono_ms, bool closing_day);
  bool emit(const Segment &seg, int64_t utc_end, bool utc_end_known, CivilDay day);
  bool write_slot(uint64_t seq, uint32_t boot, const char *json);
  bool wear_allow(uint64_t mono_ms);
  void note_wear(uint64_t mono_ms);
  bool utc_at(uint64_t mono_ms, int64_t *utc) const;
  void close_for_clock_change(uint64_t mono_ms);
  void start_segment(const AcSample &sample, bool integrable, bool zero, RecordKind kind,
                     UnavailableReason reason);
  bool queue_or_commit(const Segment &seg, int64_t utc_end, bool utc_end_known);
  bool checkpoint_from_buckets(uint64_t mono_ms);

  ByteStore *store_;
  ModelConfig cfg_;
  bool ready_;
  bool counter_trusted_;
  bool journal_untrusted_;
  bool overflow_;
  bool source_blocked_;
  bool source_qualified_;
  bool tail_open_;
  UnavailableReason blocked_reason_;
  uint64_t epoch_;
  uint64_t seq_;
  uint32_t boot_;
  uint64_t energy_;
  uint64_t residue_;
  uint64_t persisted_energy_;
  uint32_t commits_;
  uint32_t loss_;
  uint64_t allocated_;
  uint64_t unallocated_;
  CivilDay day_;
  uint64_t window_start_;
  uint32_t commits_in_window_;
  char device_id_[16];
  Segment seg_;
  Clock clock_;
  bool have_last_;
  AcSample last_;
  int32_t last_pf_;
  uint64_t day_covered_ms_;
  uint64_t day_mono_start_;
  uint32_t day_gaps_;
  bool day_measured_;
  uint32_t header_writes_;
  uint64_t durable_energy_;
  uint64_t durable_residue_;
};

}  // namespace loadac
