#pragma once

#include "byte_store.h"
#include "record_slot.h"

#include <stdint.h>

namespace powercell {

enum class SlotStatus : uint8_t { Ok, Full, Dropped, IoError, Invalid };

struct SlotStats {
  uint32_t appended = 0;
  uint32_t dropped = 0;
  uint32_t overwritten = 0;
  uint32_t truncated = 0;
  uint32_t corrupt = 0;
  uint32_t io_failures = 0;
  uint32_t erases = 0;
  uint32_t wear_stops = 0;
  uint32_t retained = 0;
  uint32_t next_seq = 1;
  uint32_t oldest_seq = 0;
  uint32_t preserve_lo = 0;
  uint32_t preserve_hi = 0;
  bool full = false;
};

struct SlotLogConfig {
  uint32_t sector_size = 256;
  bool allow_overwrite = false;
  uint32_t preserve_last = 0;
  uint32_t max_erases = 0;
};

class SlotLog {
 public:
  SlotLog(ByteStore& store, uint32_t base, uint32_t size, SlotLogConfig config);
  ~SlotLog();
  SlotLog(const SlotLog&) = delete;
  SlotLog& operator=(const SlotLog&) = delete;
  bool recover();
  SlotStatus append(const SlotRecord& record);
  bool acknowledge(uint32_t seq);
  bool read(uint32_t seq, SlotRecord* out) const;
  bool at(uint32_t index, SlotRecord* out) const;
  bool newest(SlotRecord* out) const;
  void latch_incident();
  bool release_incident();
  const SlotStats& stats() const { return stats_; }
  uint32_t capacity() const { return capacity_; }
  uint32_t bytes() const { return size_; }

 private:
  bool erase_sector(uint32_t sector_off);
  int index_of(uint32_t seq) const;
  uint32_t physical(uint32_t slot) const { return base_ + slot * kSlotBytes; }

  ByteStore& store_;
  uint32_t base_;
  uint32_t size_;
  SlotLogConfig config_;
  uint32_t capacity_ = 0;
  uint32_t* seqs_ = nullptr;
  uint8_t* valid_ = nullptr;
  SlotStats stats_;
};

struct MetaBlob {
  uint32_t epoch = 1;
  uint32_t wh = 0;
  int64_t unix_s = 0;
  uint32_t mono_ms = 0;
  uint32_t next_seq = 1;
  uint8_t clock = 0;
  uint8_t active = 0;
  uint8_t saw_missing = 0;
  uint8_t pad = 0;
};

class MetaMirror {
 public:
  MetaMirror(ByteStore& store, uint32_t sector_size);
  bool load(MetaBlob* out) const;
  bool save(const MetaBlob& blob);
  uint32_t erase_count() const { return erases_; }

 private:
  ByteStore& store_;
  uint32_t sector_;
  uint32_t erases_ = 0;
};

}  // namespace powercell
