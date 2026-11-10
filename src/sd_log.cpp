#include "sd_log.h"

#include <stdlib.h>
#include <string.h>

namespace powercell {
namespace {
constexpr uint32_t kBlockCrcAt = kSdBlockBytes - 4;

void seal_block(uint8_t block[kSdBlockBytes]) {
  put_u32(block + kBlockCrcAt, crc32_ieee(block, kBlockCrcAt));
}

bool block_blank(const uint8_t block[kSdBlockBytes]) {
  for (uint32_t i = 0; i < kSdBlockBytes; ++i) {
    if (block[i] != 0xFF) return false;
  }
  return true;
}

bool block_ok(const uint8_t block[kSdBlockBytes]) {
  if (block_blank(block)) return false;
  return get_u32(block + kBlockCrcAt) == crc32_ieee(block, kBlockCrcAt);
}
}  // namespace

SdLog::SdLog(BlockDevice& card, bool allow_overwrite)
    : card_(card), overwrite_(allow_overwrite) {
  memset(buffer_, 0xFF, sizeof(buffer_));
  index_cap_ = card.block_count() * kSdSlotsPerBlock;
  if (index_cap_ > 0) seqs_ = static_cast<uint32_t*>(calloc(index_cap_, sizeof(uint32_t)));
}

SdLog::~SdLog() { free(seqs_); }

bool SdLog::recover() {
  index_len_ = 0;
  stats_.retained = 0;
  stats_.ready = card_.present();
  buffer_dirty_ = false;
  buffer_count_ = 0;
  stats_.unsaved = 0;
  if (!seqs_ || !card_.present()) return false;
  uint32_t max_seq = 0;
  uint8_t block[kSdBlockBytes];
  uint32_t last_valid = 0;
  bool saw_valid = false;
  for (uint32_t b = 0; b < card_.block_count(); ++b) {
    if (!card_.read(b, block)) {
      ++stats_.io_failures;
      return false;
    }
    if (block_blank(block)) continue;
    if (!block_ok(block)) {
      if (!saw_valid || b > last_valid) ++stats_.truncated;
      else ++stats_.corrupt;
      continue;
    }
    saw_valid = true;
    last_valid = b;
    for (uint32_t s = 0; s < kSdSlotsPerBlock; ++s) {
      SlotRecord rec;
      if (!slot_decode(block + s * kSlotBytes, &rec)) continue;
      if (index_len_ < index_cap_) seqs_[index_len_++] = rec.seq;
      ++stats_.retained;
      if (rec.seq > max_seq) max_seq = rec.seq;
    }
  }
  stats_.next_seq = max_seq + 1;
  if (stats_.next_seq == 0) stats_.next_seq = 1;
  stats_.ready = true;
  return true;
}

int SdLog::find_seq(uint32_t seq) const {
  for (uint32_t i = 0; i < index_len_; ++i) {
    if (seqs_[i] == seq) return static_cast<int>(i);
  }
  return -1;
}

bool SdLog::commit_buffer() {
  if (!buffer_dirty_) return true;
  if (!card_.present()) {
    ++stats_.io_failures;
    stats_.ready = false;
    return false;
  }
  seal_block(buffer_);
  const uint32_t writes_before = 0;
  (void)writes_before;
  if (!card_.write(buffer_block_, buffer_) || !card_.flush()) {
    ++stats_.io_failures;
    stats_.ready = card_.present();
    return false;
  }
  ++stats_.block_writes;
  for (uint32_t i = 0; i < doomed_n_ && i < kSdSlotsPerBlock; ++i) {
    const int idx = find_seq(doomed_[i]);
    if (idx >= 0) {
      seqs_[idx] = seqs_[index_len_ - 1];
      --index_len_;
      if (stats_.retained) --stats_.retained;
      ++stats_.overwritten;
    }
  }
  doomed_n_ = 0;
  for (uint32_t s = 0; s < buffer_count_; ++s) {
    SlotRecord rec;
    if (!slot_decode(buffer_ + s * kSlotBytes, &rec)) continue;
    if (index_len_ < index_cap_) seqs_[index_len_++] = rec.seq;
    ++stats_.retained;
    ++stats_.appended;
  }
  buffer_dirty_ = false;
  buffer_count_ = 0;
  stats_.unsaved = 0;
  memset(buffer_, 0xFF, sizeof(buffer_));
  return true;
}

bool SdLog::flush() { return commit_buffer(); }

SdStatus SdLog::append(const SlotRecord& record) {
  if (record.kind == 0) return SdStatus::Invalid;
  if (!card_.present()) {
    stats_.ready = false;
    ++stats_.io_failures;
    ++stats_.dropped;
    return SdStatus::Absent;
  }
  if (buffer_count_ >= kSdSlotsPerBlock) {
    if (!commit_buffer()) return SdStatus::IoError;
  }
  if (!buffer_dirty_) {
    uint8_t block[kSdBlockBytes];
    int victim = -1;
    uint32_t victim_seq = 0xFFFFFFFFu;
    int blank = -1;
    for (uint32_t b = 0; b < card_.block_count(); ++b) {
      if (!card_.read(b, block)) {
        ++stats_.io_failures;
        return SdStatus::IoError;
      }
      if (block_blank(block)) {
        blank = static_cast<int>(b);
        break;
      }
    }
    if (blank >= 0) {
      buffer_block_ = static_cast<uint32_t>(blank);
    } else if (!overwrite_) {
      stats_.full = true;
      ++stats_.dropped;
      return SdStatus::Full;
    } else {
      for (uint32_t b = 0; b < card_.block_count(); ++b) {
        if (!card_.read(b, block) || !block_ok(block)) continue;
        SlotRecord rec;
        if (!slot_decode(block, &rec)) continue;
        if (rec.seq < victim_seq) {
          victim_seq = rec.seq;
          victim = static_cast<int>(b);
        }
      }
      if (victim < 0) {
        ++stats_.dropped;
        return SdStatus::Dropped;
      }
      if (!card_.read(static_cast<uint32_t>(victim), block)) return SdStatus::IoError;
      bool preserved = false;
      uint32_t doomed = 0;
      for (uint32_t s = 0; s < kSdSlotsPerBlock; ++s) {
        SlotRecord rec;
        if (!slot_decode(block + s * kSlotBytes, &rec)) continue;
        ++doomed;
        if (preserve_lo_ != 0 && rec.seq >= preserve_lo_ && rec.seq <= preserve_hi_) preserved = true;
      }
      if (preserved) {
        stats_.full = true;
        ++stats_.dropped;
        return SdStatus::Dropped;
      }
      buffer_block_ = static_cast<uint32_t>(victim);
      doomed_n_ = 0;
      for (uint32_t s = 0; s < kSdSlotsPerBlock; ++s) {
        SlotRecord rec;
        if (!slot_decode(block + s * kSlotBytes, &rec)) continue;
        if (doomed_n_ < kSdSlotsPerBlock) doomed_[doomed_n_++] = rec.seq;
      }
      (void)doomed;
      memset(buffer_, 0xFF, sizeof(buffer_));
    }
    buffer_dirty_ = true;
  }
  SlotRecord stored = record;
  stored.seq = stats_.next_seq++;
  if (stats_.next_seq == 0) stats_.next_seq = 1;
  slot_encode(stored, buffer_ + buffer_count_ * kSlotBytes);
  ++buffer_count_;
  stats_.unsaved = buffer_count_;
  if (buffer_count_ >= kSdSlotsPerBlock) {
    if (!commit_buffer()) return SdStatus::IoError;
  }
  return SdStatus::Ok;
}

uint32_t SdLog::collect(uint8_t kind, SlotRecord* out, uint32_t cap) {
  if (!out || cap == 0 || !card_.present() || !seqs_) return 0;
  uint32_t n = 0;
  uint8_t block[kSdBlockBytes];
  for (uint32_t b = 0; b < card_.block_count(); ++b) {
    if (!card_.read(b, block) || !block_ok(block)) continue;
    for (uint32_t s = 0; s < kSdSlotsPerBlock; ++s) {
      SlotRecord rec;
      if (!slot_decode(block + s * kSlotBytes, &rec) || rec.kind != kind) continue;
      if (n < cap) {
        out[n++] = rec;
      } else {
        uint32_t lowest = 0;
        for (uint32_t i = 1; i < n; ++i) {
          if (out[i].seq < out[lowest].seq) lowest = i;
        }
        if (rec.seq > out[lowest].seq) out[lowest] = rec;
      }
    }
  }
  for (uint32_t i = 1; i < n; ++i) {
    SlotRecord key = out[i];
    uint32_t j = i;
    while (j > 0 && out[j - 1].seq > key.seq) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = key;
  }
  return n;
}

bool SdLog::read(uint32_t seq, SlotRecord* out) const {
  if (!out || !card_.present() || find_seq(seq) < 0) return false;
  uint8_t block[kSdBlockBytes];
  for (uint32_t b = 0; b < card_.block_count(); ++b) {
    if (!card_.read(b, block) || !block_ok(block)) continue;
    for (uint32_t s = 0; s < kSdSlotsPerBlock; ++s) {
      SlotRecord rec;
      if (slot_decode(block + s * kSlotBytes, &rec) && rec.seq == seq) {
        *out = rec;
        return true;
      }
    }
  }
  return false;
}

void SdLog::latch_incident(uint32_t keep_last) {
  if (keep_last == 0 || index_len_ == 0) return;
  uint32_t hi = 0;
  for (uint32_t i = 0; i < index_len_; ++i) {
    if (seqs_[i] > hi) hi = seqs_[i];
  }
  uint32_t lo = hi;
  uint32_t left = keep_last - 1;
  while (left > 0 && lo > 1) {
    --lo;
    if (find_seq(lo) >= 0) --left;
  }
  preserve_lo_ = lo;
  preserve_hi_ = hi;
}

void SdLog::release_incident() {
  preserve_lo_ = 0;
  preserve_hi_ = 0;
  stats_.full = false;
}

FakeCard::FakeCard(uint32_t blocks)
    : blocks_(blocks), data_(static_cast<uint8_t*>(malloc(blocks * kSdBlockBytes))) {
  if (data_) memset(data_, 0xFF, blocks * kSdBlockBytes);
}

FakeCard::~FakeCard() { free(data_); }

bool FakeCard::read(uint32_t block, uint8_t* dst) {
  if (!present_ || !data_ || !dst || block >= blocks_) return false;
  memcpy(dst, data_ + block * kSdBlockBytes, kSdBlockBytes);
  return true;
}

bool FakeCard::write(uint32_t block, const uint8_t* src) {
  ++writes_;
  if (!present_ || !data_ || !src || block >= blocks_) return false;
  if (fail_) {
    fail_ = false;
    return false;
  }
  if (tear_) {
    tear_ = false;
    memcpy(data_ + block * kSdBlockBytes, src, 16);
    memset(data_ + block * kSdBlockBytes + 16, 0xA5, 32);
    return false;
  }
  memcpy(data_ + block * kSdBlockBytes, src, kSdBlockBytes);
  return true;
}

bool FakeCard::flush() { return present_; }

void FakeCard::set_present(bool present) { present_ = present; }

void FakeCard::fail_next_write() { fail_ = true; }

void FakeCard::tear_next_write() { tear_ = true; }

void FakeCard::corrupt_block(uint32_t block) {
  if (!data_ || block >= blocks_) return;
  data_[block * kSdBlockBytes + 20] ^= 0x5A;
}

}  // namespace powercell
