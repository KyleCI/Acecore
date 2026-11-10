#pragma once

#include "ds3231_time.h"
#include "slot_log.h"

#include <stdint.h>

namespace powercell {

struct EnergyConfig {
  uint32_t bucket_s = 900;
  uint32_t max_power_w = 26000;
  uint32_t max_unbounded_wh = 26000;
  uint32_t midnight_margin_s = 180;
};

enum class EnergyStatus : uint8_t {
  Baseline = 0,
  Missing,
  Open,
  Closed,
  Unallocated,
  Discontinuity,
  OutOfOrder,
  StoreFull,
  NotSaved,
};

struct MeterSample {
  bool valid = false;
  uint32_t lifetime_wh = 0;
  uint32_t mono_ms = 0;
  bool mono_continuous = false;
  int64_t unix_s = 0;
  uint8_t clock = 0;
  uint32_t margin_s = 0;
  bool measurement_gap = false;
};

struct EnergyView {
  uint32_t seq = 0;
  uint32_t epoch = 0;
  uint32_t start_wh = 0;
  uint32_t end_wh = 0;
  uint32_t delta_wh = 0xFFFFFFFFu;
  int64_t start_unix = 0;
  int64_t end_unix = 0;
  int32_t civil_day = 0;
  uint8_t kind = 0;
  uint8_t allocation = 0;
  uint8_t measurement = 0;
  uint8_t version = 1;
  uint32_t subject = 0;
  bool acked = false;
};

class EnergyLog {
 public:
  EnergyLog(SlotLog& slots, MetaMirror& meta, EnergyConfig config);
  ~EnergyLog();
  EnergyLog(const EnergyLog&) = delete;
  EnergyLog& operator=(const EnergyLog&) = delete;

  bool load();
  EnergyStatus observe(const MeterSample& sample);
  bool acknowledge(uint32_t seq, bool durable_commit);
  bool note_anchor(int64_t rtc_unix, int64_t ntp_unix, int64_t uncertainty_s);
  void set_persist_meta(bool enabled) { persist_meta_ = enabled; }

  bool requests_meter_reset() const { return false; }
  bool durable() const { return durable_; }
  bool store_full() const { return slots_.stats().full; }
  uint32_t epoch() const { return epoch_; }
  uint32_t baseline_wh() const { return open_wh_; }
  bool baseline_valid() const { return open_active_; }
  uint32_t sum_allocated() const;
  uint32_t sum_unallocated() const;
  uint32_t record_count() const { return summary_count_; }
  bool view_at(uint32_t index, EnergyView* out) const;

 private:
  bool remember(const EnergyView& view);
  bool write_view(const EnergyView& view);
  int effective_day(uint32_t seq, int original) const;
  void adopt(const MeterSample& sample);
  bool save_meta();

  SlotLog& slots_;
  MetaMirror& meta_;
  EnergyConfig config_;
  EnergyView* summaries_ = nullptr;
  uint32_t summary_cap_ = 0;
  uint32_t summary_count_ = 0;
  bool open_active_ = false;
  bool open_mono_this_boot_ = false;
  bool saw_missing_ = false;
  bool durable_ = true;
  bool persist_meta_ = true;
  uint32_t epoch_ = 1;
  uint32_t open_wh_ = 0;
  uint32_t open_mono_ = 0;
  uint32_t open_margin_ = 0;
  int64_t open_unix_ = 0;
  uint8_t open_clock_ = 0;
};

}  // namespace powercell
