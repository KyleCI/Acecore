#include "slot_log.h"

#include <stdlib.h>
#include <string.h>

namespace powercell {
namespace {
constexpr uint32_t kMetaMagic = 0x3143454Du;  // 'MEC1' little-endian bytes M E C 1

void encode_meta(const MetaBlob& in, uint32_t generation, uint8_t raw[128]) {
  memset(raw, 0xFF, 128);
  put_u32(raw + 0, kMetaMagic);
  put_u32(raw + 4, generation);
  put_u32(raw + 8, in.epoch);
  put_u32(raw + 12, in.wh);
  put_i64(raw + 16, in.unix_s);
  put_u32(raw + 24, in.mono_ms);
  put_u32(raw + 28, in.next_seq);
  raw[32] = in.clock;
  raw[33] = in.active;
  raw[34] = in.saw_missing;
  raw[35] = 0;
  put_u32(raw + 36, crc32_ieee(raw, 36));
}

bool decode_meta(const uint8_t raw[128], MetaBlob* out, uint32_t* generation) {
  if (get_u32(raw) != kMetaMagic) return false;
  if (get_u32(raw + 36) != crc32_ieee(raw, 36)) return false;
  *generation = get_u32(raw + 4);
  out->epoch = get_u32(raw + 8);
  out->wh = get_u32(raw + 12);
  out->unix_s = get_i64(raw + 16);
  out->mono_ms = get_u32(raw + 24);
  out->next_seq = get_u32(raw + 28);
  out->clock = raw[32];
  out->active = raw[33];
  out->saw_missing = raw[34];
  return out->next_seq != 0;
}
}  // namespace

SlotLog::SlotLog(ByteStore& store, uint32_t base, uint32_t size, SlotLogConfig config)
    : store_(store), base_(base), size_(size), config_(config) {
  if (config_.sector_size < kSlotBytes || (config_.sector_size % kSlotBytes) != 0) return;
  if (size < kSlotBytes || base + size > store.size()) return;
  capacity_ = size / kSlotBytes;
  seqs_ = static_cast<uint32_t*>(calloc(capacity_, sizeof(uint32_t)));
  valid_ = static_cast<uint8_t*>(calloc(capacity_, 1));
}

SlotLog::~SlotLog() {
  free(seqs_);
  free(valid_);
}

bool SlotLog::recover() {
  if (!seqs_ || !valid_) return false;
  memset(seqs_, 0, capacity_ * sizeof(uint32_t));
  memset(valid_, 0, capacity_);
  stats_.retained = 0;
  stats_.oldest_seq = 0;
  uint32_t max_seq = 0;
  uint8_t raw[kSlotBytes];
  uint32_t highest_occupied = 0;
  bool any_occupied = false;
  for (uint32_t i = 0; i < capacity_; ++i) {
    if (!store_.read(physical(i), raw, kSlotBytes)) return false;
    if (slot_is_blank(raw)) continue;
    any_occupied = true;
    highest_occupied = i;
    SlotRecord rec;
    if (!slot_decode(raw, &rec)) {
      const bool tail = true;
      (void)tail;
      ++stats_.truncated;
      continue;
    }
    seqs_[i] = rec.seq;
    valid_[i] = 1;
    ++stats_.retained;
    if (stats_.oldest_seq == 0 || rec.seq < stats_.oldest_seq) stats_.oldest_seq = rec.seq;
    if (rec.seq > max_seq) max_seq = rec.seq;
  }
  // A bad slot after the last valid one is a torn tail. A bad slot with a
  // later valid slot is corruption in the middle. Recount with that split.
  if (any_occupied) {
    uint32_t bad_tail = 0;
    uint32_t bad_mid = 0;
    uint32_t last_valid = 0;
    bool saw_valid = false;
    for (uint32_t i = 0; i < capacity_; ++i) {
      if (valid_[i]) {
        saw_valid = true;
        last_valid = i;
      }
    }
    for (uint32_t i = 0; i < capacity_; ++i) {
      if (!store_.read(physical(i), raw, kSlotBytes)) return false;
      if (slot_is_blank(raw) || valid_[i]) continue;
      if (!saw_valid || i > last_valid) ++bad_tail;
      else ++bad_mid;
    }
    stats_.truncated = bad_tail;
    stats_.corrupt = bad_mid;
    (void)highest_occupied;
  }
  stats_.next_seq = max_seq + 1;
  if (stats_.next_seq == 0) stats_.next_seq = 1;
  stats_.full = !config_.allow_overwrite && stats_.retained == capacity_;
  return true;
}

int SlotLog::index_of(uint32_t seq) const {
  for (uint32_t i = 0; i < capacity_; ++i) {
    if (valid_[i] && seqs_[i] == seq) return static_cast<int>(i);
  }
  return -1;
}

bool SlotLog::erase_sector(uint32_t sector_off) {
  if (config_.max_erases != 0 && stats_.erases >= config_.max_erases) {
    ++stats_.wear_stops;
    return false;
  }
  if (!store_.erase(base_ + sector_off, config_.sector_size)) {
    ++stats_.io_failures;
    return false;
  }
  ++stats_.erases;
  const uint32_t first = sector_off / kSlotBytes;
  const uint32_t n = config_.sector_size / kSlotBytes;
  for (uint32_t i = 0; i < n && first + i < capacity_; ++i) {
    if (valid_[first + i]) {
      valid_[first + i] = 0;
      seqs_[first + i] = 0;
      if (stats_.retained) --stats_.retained;
      ++stats_.overwritten;
    }
  }
  stats_.oldest_seq = 0;
  for (uint32_t i = 0; i < capacity_; ++i) {
    if (!valid_[i]) continue;
    if (stats_.oldest_seq == 0 || seqs_[i] < stats_.oldest_seq) stats_.oldest_seq = seqs_[i];
  }
  return true;
}

SlotStatus SlotLog::append(const SlotRecord& record) {
  if (!seqs_ || record.kind == 0) return SlotStatus::Invalid;
  int free_slot = -1;
  for (uint32_t i = 0; i < capacity_; ++i) {
    uint8_t raw[kSlotBytes];
    if (!store_.read(physical(i), raw, kSlotBytes)) {
      ++stats_.io_failures;
      return SlotStatus::IoError;
    }
    if (slot_is_blank(raw) && !valid_[i]) {
      free_slot = static_cast<int>(i);
      break;
    }
  }
  if (free_slot < 0) {
    if (!config_.allow_overwrite) {
      stats_.full = true;
      ++stats_.dropped;
      return SlotStatus::Full;
    }
    uint32_t oldest = 0;
    bool found = false;
    for (uint32_t i = 0; i < capacity_; ++i) {
      if (!valid_[i]) continue;
      if (!found || seqs_[i] < seqs_[oldest]) {
        oldest = i;
        found = true;
      }
    }
    if (!found) return SlotStatus::IoError;
    if (stats_.preserve_lo != 0 && seqs_[oldest] >= stats_.preserve_lo &&
        seqs_[oldest] <= stats_.preserve_hi) {
      ++stats_.dropped;
      stats_.full = true;
      return SlotStatus::Dropped;
    }
    const uint32_t sector_off = (oldest * kSlotBytes / config_.sector_size) * config_.sector_size;
    if (!erase_sector(sector_off)) {
      ++stats_.dropped;
      return SlotStatus::Dropped;
    }
    free_slot = static_cast<int>(sector_off / kSlotBytes);
  }
  SlotRecord stored = record;
  stored.seq = stats_.next_seq;
  stored.acked = false;
  uint8_t raw[kSlotBytes];
  slot_encode(stored, raw);
  if (!store_.write(physical(static_cast<uint32_t>(free_slot)), raw, kSlotBytes) || !store_.flush()) {
    ++stats_.io_failures;
    return SlotStatus::IoError;
  }
  seqs_[free_slot] = stored.seq;
  valid_[free_slot] = 1;
  ++stats_.retained;
  ++stats_.appended;
  ++stats_.next_seq;
  if (stats_.oldest_seq == 0 || stored.seq < stats_.oldest_seq) stats_.oldest_seq = stored.seq;
  stats_.full = !config_.allow_overwrite && stats_.retained == capacity_;
  return SlotStatus::Ok;
}

bool SlotLog::acknowledge(uint32_t seq) {
  const int idx = index_of(seq);
  if (idx < 0) return false;
  uint8_t raw[kSlotBytes];
  if (!store_.read(physical(static_cast<uint32_t>(idx)), raw, kSlotBytes)) return false;
  if (raw[100] == 0xFF) {
    raw[100] = 0xFE;
    if (!store_.write(physical(static_cast<uint32_t>(idx)) + 100, raw + 100, 1)) return false;
  }
  return true;
}

bool SlotLog::read(uint32_t seq, SlotRecord* out) const {
  const int idx = index_of(seq);
  if (idx < 0 || !out) return false;
  uint8_t raw[kSlotBytes];
  if (!store_.read(physical(static_cast<uint32_t>(idx)), raw, kSlotBytes)) return false;
  return slot_decode(raw, out);
}

bool SlotLog::at(uint32_t index, SlotRecord* out) const {
  if (!out || index >= capacity_ || !valid_[index]) return false;
  return read(seqs_[index], out);
}

bool SlotLog::newest(SlotRecord* out) const {
  uint32_t best = 0;
  int idx = -1;
  for (uint32_t i = 0; i < capacity_; ++i) {
    if (valid_[i] && seqs_[i] >= best) {
      best = seqs_[i];
      idx = static_cast<int>(i);
    }
  }
  if (idx < 0) return false;
  return read(best, out);
}

void SlotLog::latch_incident() {
  if (stats_.retained == 0 || config_.preserve_last == 0) return;
  uint32_t hi = 0;
  for (uint32_t i = 0; i < capacity_; ++i) {
    if (valid_[i] && seqs_[i] > hi) hi = seqs_[i];
  }
  uint32_t lo = hi;
  uint32_t left = config_.preserve_last - 1;
  while (left > 0 && lo > 1) {
    if (index_of(lo - 1) >= 0) --left;
    --lo;
    if (lo == 1) break;
  }
  stats_.preserve_lo = lo;
  stats_.preserve_hi = hi;
}

bool SlotLog::release_incident() {
  stats_.preserve_lo = 0;
  stats_.preserve_hi = 0;
  stats_.full = false;
  return true;
}

MetaMirror::MetaMirror(ByteStore& store, uint32_t sector_size)
    : store_(store), sector_(sector_size) {}

bool MetaMirror::load(MetaBlob* out) const {
  if (!out || sector_ < 128 || store_.size() < sector_ * 2) return false;
  MetaBlob best;
  uint32_t best_gen = 0;
  bool any = false;
  uint8_t raw[128];
  for (uint32_t copy = 0; copy < 2; ++copy) {
    if (!store_.read(copy * sector_, raw, 128)) return false;
    MetaBlob blob;
    uint32_t gen = 0;
    if (!decode_meta(raw, &blob, &gen)) continue;
    if (!any || gen >= best_gen) {
      best = blob;
      best_gen = gen;
      any = true;
    }
  }
  if (!any) return false;
  *out = best;
  return true;
}

bool MetaMirror::save(const MetaBlob& blob) {
  if (sector_ < 128 || store_.size() < sector_ * 2) return false;
  uint32_t gen = 1;
  MetaBlob current;
  uint8_t raw[128];
  for (uint32_t copy = 0; copy < 2; ++copy) {
    uint32_t have = 0;
    if (store_.read(copy * sector_, raw, 128) && decode_meta(raw, &current, &have) && have >= gen) {
      gen = have + 1;
    }
  }
  const uint32_t copy = gen & 1u;
  if (!store_.erase(copy * sector_, sector_)) return false;
  ++erases_;
  encode_meta(blob, gen, raw);
  if (!store_.write(copy * sector_, raw, 128)) return false;
  uint8_t check[128];
  if (!store_.read(copy * sector_, check, 128)) return false;
  return memcmp(raw, check, 128) == 0;
}

}  // namespace powercell
