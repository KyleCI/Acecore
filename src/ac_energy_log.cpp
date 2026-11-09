#include "ac_energy_log.h"

#include "lagos_time.h"

#include <stdlib.h>
#include <string.h>

namespace powercell {
namespace {
constexpr uint32_t kNoDelta = 0xFFFFFFFFu;
constexpr uint8_t kAllocDay = 1;
constexpr uint8_t kAllocNone = 0;
constexpr uint8_t kMeasured = 0;
constexpr uint8_t kGap = 1;
void put_view(const EnergyView& view, uint8_t payload[kSlotPayload]) {
  memset(payload, 0, kSlotPayload);
  payload[0] = view.allocation;
  payload[1] = view.measurement;
  payload[2] = 0;
  payload[3] = 0;
  payload[4] = view.version;
  payload[5] = 0;
  payload[6] = view.measurement;
  put_u32(payload + 8, view.epoch);
  put_u32(payload + 12, view.start_wh);
  put_u32(payload + 16, view.end_wh);
  put_u32(payload + 20, view.delta_wh);
  put_u32(payload + 24, view.subject);
  put_i32(payload + 28, view.civil_day);
  put_i64(payload + 32, view.start_unix);
  put_i64(payload + 40, view.end_unix);
}

bool get_view(const SlotRecord& rec, EnergyView* view) {
  if (rec.len < 48 && rec.kind != KindEnergyInterval && rec.kind != KindEnergyDiscontinuity &&
      rec.kind != KindEnergyCorrection) {
    return false;
  }
  const uint8_t* p = rec.payload;
  view->seq = rec.seq;
  view->kind = rec.kind;
  view->allocation = p[0];
  view->measurement = p[1];
  view->version = p[4] == 0 ? 1 : p[4];
  view->epoch = get_u32(p + 8);
  view->start_wh = get_u32(p + 12);
  view->end_wh = get_u32(p + 16);
  view->delta_wh = get_u32(p + 20);
  view->subject = get_u32(p + 24);
  view->civil_day = get_i32(p + 28);
  view->start_unix = get_i64(p + 32);
  view->end_unix = get_i64(p + 40);
  view->acked = rec.acked;
  return true;
}

}  // namespace

EnergyLog::EnergyLog(SlotLog& slots, MetaMirror& meta, EnergyConfig config)
    : slots_(slots), meta_(meta), config_(config) {
  summary_cap_ = slots.capacity() + 8;
  if (summary_cap_ < 8) summary_cap_ = 8;
  summaries_ = static_cast<EnergyView*>(calloc(summary_cap_, sizeof(EnergyView)));
}

EnergyLog::~EnergyLog() { free(summaries_); }

bool EnergyLog::remember(const EnergyView& view) {
  if (!summaries_ || summary_count_ >= summary_cap_) return false;
  summaries_[summary_count_++] = view;
  return true;
}

bool EnergyLog::view_at(uint32_t index, EnergyView* out) const {
  if (!out || index >= summary_count_) return false;
  *out = summaries_[index];
  return true;
}

int EnergyLog::effective_day(uint32_t seq, int original) const {
  int day = original;
  uint8_t version = 0;
  for (uint32_t i = 0; i < summary_count_; ++i) {
    const EnergyView& v = summaries_[i];
    if (v.kind == KindEnergyCorrection && v.subject == seq && v.version >= version) {
      version = v.version;
      day = v.civil_day;
    }
  }
  return day;
}

uint32_t EnergyLog::sum_allocated() const {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < summary_count_; ++i) {
    const EnergyView& v = summaries_[i];
    if (v.kind != KindEnergyInterval || v.delta_wh == kNoDelta) continue;
    if (effective_day(v.seq, v.civil_day) > 0) sum += v.delta_wh;
  }
  return sum;
}

uint32_t EnergyLog::sum_unallocated() const {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < summary_count_; ++i) {
    const EnergyView& v = summaries_[i];
    if (v.kind != KindEnergyInterval || v.delta_wh == kNoDelta) continue;
    if (effective_day(v.seq, v.civil_day) <= 0) sum += v.delta_wh;
  }
  return sum;
}

bool EnergyLog::save_meta() {
  if (!persist_meta_) return true;
  MetaBlob blob;
  blob.epoch = epoch_;
  blob.wh = open_wh_;
  blob.unix_s = open_unix_;
  blob.mono_ms = open_mono_;
  blob.next_seq = slots_.stats().next_seq;
  blob.clock = open_clock_;
  blob.active = open_active_ ? 1 : 0;
  blob.saw_missing = saw_missing_ ? 1 : 0;
  return meta_.save(blob);
}

bool EnergyLog::load() {
  summary_count_ = 0;
  open_active_ = false;
  open_mono_this_boot_ = false;
  if (!slots_.recover()) {
    durable_ = false;
    return false;
  }
  MetaBlob meta;
  const bool have_meta = meta_.load(&meta);
  if (have_meta && meta.active) {
    open_active_ = true;
    epoch_ = meta.epoch == 0 ? 1 : meta.epoch;
    open_wh_ = meta.wh;
    open_unix_ = meta.unix_s;
    open_mono_ = meta.mono_ms;
    open_clock_ = meta.clock;
    saw_missing_ = meta.saw_missing != 0;
  }
  for (uint32_t i = 0; i < slots_.capacity(); ++i) {
    SlotRecord rec;
    if (!slots_.at(i, &rec)) continue;
    if (rec.kind != KindEnergyInterval && rec.kind != KindEnergyDiscontinuity &&
        rec.kind != KindEnergyCorrection) {
      continue;
    }
    EnergyView view;
    if (!get_view(rec, &view)) continue;
    // Clocks live in the record header, not the payload.
    view.allocation = rec.payload[0];
    remember(view);
  }
  // Replay each saved record at most once so a zero-delta interval cannot loop.
  uint8_t* used = static_cast<uint8_t*>(calloc(summary_count_ ? summary_count_ : 1, 1));
  bool progressed = true;
  while (progressed && used) {
    progressed = false;
    for (uint32_t i = 0; i < summary_count_; ++i) {
      if (used[i]) continue;
      const EnergyView& v = summaries_[i];
      if (v.kind != KindEnergyInterval && v.kind != KindEnergyDiscontinuity) continue;
      if (!open_active_) {
        used[i] = 1;
        open_active_ = true;
        epoch_ = v.epoch == 0 ? 1 : v.epoch;
        open_wh_ = v.end_wh;
        open_unix_ = v.end_unix;
        open_clock_ = 0;
        progressed = true;
        continue;
      }
      if (v.kind == KindEnergyInterval && v.epoch == epoch_ && v.start_wh == open_wh_) {
        used[i] = 1;
        open_wh_ = v.end_wh;
        open_unix_ = v.end_unix;
        progressed = true;
      } else if (v.kind == KindEnergyDiscontinuity && v.start_wh == open_wh_) {
        used[i] = 1;
        epoch_ = v.epoch;
        open_wh_ = v.end_wh;
        open_unix_ = v.end_unix;
        progressed = true;
      }
    }
  }
  free(used);
  durable_ = true;
  return true;
}

void EnergyLog::adopt(const MeterSample& sample) {
  open_active_ = true;
  open_wh_ = sample.lifetime_wh;
  open_unix_ = sample.unix_s;
  open_mono_ = sample.mono_ms;
  open_clock_ = sample.clock;
  open_margin_ = sample.margin_s;
  open_mono_this_boot_ = sample.mono_continuous;
  saw_missing_ = false;
}

bool EnergyLog::write_view(const EnergyView& view) {
  SlotRecord rec;
  rec.kind = view.kind;
  rec.mono_ms = open_mono_;
  rec.source_unix = view.start_unix;
  rec.recv_unix = view.end_unix;
  rec.clock_quality = open_clock_;
  rec.flags = view.measurement == kGap ? FlagMeasurementGap : 0;
  rec.len = 48;
  put_view(view, rec.payload);
  rec.payload[2] = open_clock_;
  const SlotStatus status = slots_.append(rec);
  if (status == SlotStatus::Full) return false;
  if (status != SlotStatus::Ok) {
    durable_ = false;
    return false;
  }
  EnergyView stored = view;
  stored.seq = slots_.stats().next_seq - 1;
  remember(stored);
  durable_ = true;
  return true;
}

EnergyStatus EnergyLog::observe(const MeterSample& sample) {
  if (!sample.valid) {
    saw_missing_ = true;
    return EnergyStatus::Missing;
  }
  if (!open_active_) {
    adopt(sample);
    epoch_ = epoch_ == 0 ? 1 : epoch_;
    save_meta();
    return EnergyStatus::Baseline;
  }
  if (sample.mono_continuous && open_mono_this_boot_ &&
      static_cast<int32_t>(sample.mono_ms - open_mono_) < 0) {
    return EnergyStatus::OutOfOrder;
  }
  const bool mono_known = sample.mono_continuous && open_mono_this_boot_;
  const bool unix_known = sample.unix_s > 0 && open_unix_ > 0 && sample.unix_s >= open_unix_;
  uint32_t elapsed = 0;
  bool time_known = false;
  if (mono_known || unix_known) {
    const uint32_t mono_elapsed = mono_known ? sample.mono_ms - open_mono_ : 0xFFFFFFFFu;
    const uint32_t unix_elapsed =
        unix_known ? static_cast<uint32_t>(sample.unix_s - open_unix_) : 0xFFFFFFFFu;
    elapsed = mono_elapsed < unix_elapsed ? mono_elapsed : unix_elapsed;
    time_known = true;
  }
  if (sample.lifetime_wh < open_wh_) {
    EnergyView view;
    view.kind = KindEnergyDiscontinuity;
    view.epoch = epoch_ + 1;
    view.start_wh = open_wh_;
    view.end_wh = sample.lifetime_wh;
    view.delta_wh = kNoDelta;
    view.start_unix = open_unix_;
    view.end_unix = sample.unix_s;
    view.allocation = kAllocNone;
    if (!write_view(view)) return slots_.stats().full ? EnergyStatus::StoreFull : EnergyStatus::NotSaved;
    epoch_ = view.epoch;
    adopt(sample);
    save_meta();
    return EnergyStatus::Discontinuity;
  }
  const uint32_t delta = sample.lifetime_wh - open_wh_;
  const uint64_t cap = time_known
                           ? (static_cast<uint64_t>(config_.max_power_w) * elapsed) / 3600ull + 2ull
                           : config_.max_unbounded_wh;
  if (static_cast<uint64_t>(delta) > cap) {
    EnergyView view;
    view.kind = KindEnergyDiscontinuity;
    view.epoch = epoch_ + 1;
    view.start_wh = open_wh_;
    view.end_wh = sample.lifetime_wh;
    view.delta_wh = kNoDelta;
    view.start_unix = open_unix_;
    view.end_unix = sample.unix_s;
    if (!write_view(view)) return slots_.stats().full ? EnergyStatus::StoreFull : EnergyStatus::NotSaved;
    epoch_ = view.epoch;
    adopt(sample);
    save_meta();
    return EnergyStatus::Discontinuity;
  }
  const bool gap = saw_missing_ || sample.measurement_gap;
  const bool bucket = time_known && elapsed >= config_.bucket_s;
  const int start_day = open_clock_ == static_cast<uint8_t>(ClockQuality::Anchored) ? lagos_day_id(open_unix_) : 0;
  const int end_day = sample.clock == static_cast<uint8_t>(ClockQuality::Anchored) ? lagos_day_id(sample.unix_s) : 0;
  const bool day_cross = start_day != 0 && end_day != 0 && start_day != end_day;
  if (!gap && !bucket && !day_cross) return EnergyStatus::Open;
  const uint32_t margin = open_margin_ > sample.margin_s ? open_margin_ : sample.margin_s;
  bool allocate = start_day != 0 && start_day == end_day &&
                  !lagos_interval_crosses_midnight(open_unix_, sample.unix_s, margin);
  if (!time_known) allocate = false;
  EnergyView view;
  view.kind = KindEnergyInterval;
  view.epoch = epoch_;
  view.start_wh = open_wh_;
  view.end_wh = sample.lifetime_wh;
  view.delta_wh = delta;
  view.start_unix = open_unix_;
  view.end_unix = sample.unix_s;
  view.measurement = gap ? kGap : kMeasured;
  view.allocation = allocate ? kAllocDay : kAllocNone;
  view.civil_day = allocate ? start_day : 0;
  if (!write_view(view)) return slots_.stats().full ? EnergyStatus::StoreFull : EnergyStatus::NotSaved;
  adopt(sample);
  save_meta();
  return allocate ? EnergyStatus::Closed : EnergyStatus::Unallocated;
}

bool EnergyLog::acknowledge(uint32_t seq, bool durable_commit) {
  if (!durable_commit) return false;
  if (!slots_.acknowledge(seq)) return false;
  for (uint32_t i = 0; i < summary_count_; ++i) {
    if (summaries_[i].seq == seq) summaries_[i].acked = true;
  }
  return true;
}

bool EnergyLog::note_anchor(int64_t rtc_unix, int64_t ntp_unix, int64_t uncertainty_s) {
  if (rtc_unix <= 0 || ntp_unix <= 0 || uncertainty_s < 0) return false;
  const int64_t offset = rtc_unix - ntp_unix;
  bool any = false;
  const uint32_t count = summary_count_;
  for (uint32_t i = 0; i < count; ++i) {
    const EnergyView& v = summaries_[i];
    if (v.kind != KindEnergyInterval || v.delta_wh == kNoDelta) continue;
    if (effective_day(v.seq, v.civil_day) > 0) continue;
    if (v.start_unix <= 0 || v.end_unix < v.start_unix) continue;
    const int64_t start = v.start_unix - offset - uncertainty_s;
    const int64_t end = v.end_unix - offset + uncertainty_s;
    if (start <= 0 || end < start) continue;
    if (lagos_day_id(start) == 0 || lagos_day_id(start) != lagos_day_id(end)) continue;
    if (lagos_interval_crosses_midnight(start, end, 0)) continue;
    EnergyView fix;
    fix.kind = KindEnergyCorrection;
    fix.subject = v.seq;
    fix.version = static_cast<uint8_t>(v.version + 1);
    fix.civil_day = lagos_day_id(start);
    fix.allocation = kAllocDay;
    fix.delta_wh = kNoDelta;
    fix.epoch = v.epoch;
    fix.start_wh = v.start_wh;
    fix.end_wh = v.end_wh;
    fix.start_unix = v.start_unix - offset;
    fix.end_unix = v.end_unix - offset;
    if (!write_view(fix)) return false;
    any = true;
  }
  return any;
}

}  // namespace powercell
