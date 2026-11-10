#pragma once

#include "sd_log.h"

#include <stddef.h>
#include <stdint.h>

namespace powercell {

// Copies ESP32 console bytes into a bounded queue. It does not write the
// console and it does not touch the caller's buffer. SD writes happen only
// in drain(), so the byte path cannot block on the card.
class SerialSpool {
 public:
  static constexpr int kChunkText = 40;
  static constexpr int kMaxChunks = 6;
  static constexpr int kQueue = 12;
  static constexpr int kRecent = 16;

  void set_suppressed(bool suppressed);
  bool suppressed() const { return suppressed_; }
  void ingest(const char* data, size_t n);
  uint32_t drain(SdLog* sd, int max_records);
  int export_sd(SdLog* sd, char* out, size_t cap);
  int export_recent(char* out, size_t cap) const;
  uint32_t dropped_bytes() const { return dropped_bytes_; }
  uint32_t queued() const { return qn_; }

 private:
  void commit_line(bool truncated);
  bool enqueue(const char* text);

  char assem_[kChunkText * kMaxChunks + 1] = {};
  uint16_t assem_len_ = 0;
  char queue_[kQueue][64] = {};
  uint8_t qh_ = 0;
  uint8_t qn_ = 0;
  char recent_[kRecent][64] = {};
  uint8_t recent_n_ = 0;
  uint8_t recent_pos_ = 0;
  uint32_t dropped_bytes_ = 0;
  bool suppressed_ = false;
};

SerialSpool& console_spool();

}  // namespace powercell
