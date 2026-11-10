#pragma once

#include "record_slot.h"

#include <stdint.h>

namespace powercell {

// 512-byte card blocks. Records are buffered and committed as a whole block.
// A torn or missing commit loses at most that one block (4 records) plus the
// RAM buffer. This is not a FAT filesystem; FAT remains a fitted-board question.
constexpr uint32_t kSdBlockBytes = 512;
constexpr uint32_t kSdSlotsPerBlock = kSdBlockBytes / kSlotBytes;

class BlockDevice {
 public:
  virtual ~BlockDevice() {}
  virtual uint32_t block_count() const = 0;
  virtual bool present() const = 0;
  virtual bool read(uint32_t block, uint8_t* dst) = 0;
  virtual bool write(uint32_t block, const uint8_t* src) = 0;
  virtual bool flush() = 0;
};

enum class SdStatus : uint8_t { Ok, Full, Dropped, IoError, Absent, Invalid };

struct SdStats {
  uint32_t appended = 0;
  uint32_t dropped = 0;
  uint32_t overwritten = 0;
  uint32_t truncated = 0;
  uint32_t corrupt = 0;
  uint32_t io_failures = 0;
  uint32_t block_writes = 0;
  uint32_t unsaved = 0;
  uint32_t retained = 0;
  uint32_t next_seq = 1;
  bool ready = false;
  bool full = false;
};

class SdLog {
 public:
  explicit SdLog(BlockDevice& card, bool allow_overwrite);
  ~SdLog();
  SdLog(const SdLog&) = delete;
  SdLog& operator=(const SdLog&) = delete;
  bool recover();
  SdStatus append(const SlotRecord& record);
  bool flush();
  bool read(uint32_t seq, SlotRecord* out) const;
  // Newest committed records of one kind, oldest of that selection first.
  uint32_t collect(uint8_t kind, SlotRecord* out, uint32_t cap);
  void latch_incident(uint32_t keep_last);
  void release_incident();
  const SdStats& stats() const { return stats_; }

 private:
  bool commit_buffer();
  int find_seq(uint32_t seq) const;

  BlockDevice& card_;
  bool overwrite_;
  uint8_t buffer_[kSdBlockBytes];
  uint32_t buffer_block_ = 0;
  uint32_t buffer_count_ = 0;
  uint32_t doomed_[kSdSlotsPerBlock] = {};
  uint32_t doomed_n_ = 0;
  bool buffer_dirty_ = false;
  uint32_t* seqs_ = nullptr;
  uint32_t index_cap_ = 0;
  uint32_t index_len_ = 0;
  uint32_t preserve_lo_ = 0;
  uint32_t preserve_hi_ = 0;
  SdStats stats_;
};

class FakeCard : public BlockDevice {
 public:
  explicit FakeCard(uint32_t blocks);
  ~FakeCard() override;
  uint32_t block_count() const override { return blocks_; }
  bool present() const override { return present_; }
  bool read(uint32_t block, uint8_t* dst) override;
  bool write(uint32_t block, const uint8_t* src) override;
  bool flush() override;
  void set_present(bool present);
  void fail_next_write();
  void tear_next_write();
  void corrupt_block(uint32_t block);
  uint32_t write_count() const { return writes_; }

 private:
  uint32_t blocks_;
  uint8_t* data_;
  bool present_ = true;
  bool fail_ = false;
  bool tear_ = false;
  uint32_t writes_ = 0;
};

}  // namespace powercell
