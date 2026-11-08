#pragma once

#include "board_pins.h"
#include "record_slot.h"
#include "sd_log.h"
#include "slot_log.h"

#include <stdint.h>

namespace powercell {

class DiagQueue {
 public:
  explicit DiagQueue(uint32_t capacity);
  ~DiagQueue();
  bool try_push(const SlotRecord& record);
  bool pop(SlotRecord* out);
  uint32_t dropped() const { return dropped_; }
  uint32_t size() const { return size_; }

 private:
  SlotRecord* items_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t head_ = 0;
  uint32_t size_ = 0;
  uint32_t dropped_ = 0;
  int depth_ = 0;
};

class ConsoleCapture {
 public:
  typedef void (*Sink)(const char* line, bool truncated, void* ctx);
  ConsoleCapture(Sink sink, void* ctx);
  void byte(char c);
  uint32_t dropped() const { return dropped_; }

 private:
  Sink sink_;
  void* ctx_;
  char line_[96];
  uint8_t len_ = 0;
  bool truncated_ = false;
  int depth_ = 0;
  uint32_t dropped_ = 0;
};

class UploadCursor {
 public:
  void set_contract(bool enabled) { contract_ = enabled; }
  bool contract() const { return contract_; }
  uint32_t acked() const { return acked_; }
  bool plan(const uint32_t* seqs, uint32_t count, uint32_t now_ms, uint16_t max_n,
            uint32_t* first, uint32_t* last, uint16_t* n);
  void on_transport_success();
  void on_ack(uint32_t seq, bool durable_commit, uint32_t now_ms);
  void on_failure(uint32_t now_ms);

 private:
  uint32_t acked_ = 0;
  uint32_t backoff_until_ = 0;
  uint32_t backoff_ms_ = 1000;
  bool contract_ = false;
};

int redact_text(char* text, size_t cap);
int format_diag_text(const SlotRecord& record, char* out, size_t cap);
SlotStatus route_diag(HardwareProfile profile, SdLog* sd, SlotLog* flash, const SlotRecord& record);

}  // namespace powercell
