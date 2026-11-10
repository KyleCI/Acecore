#include "diag_blackbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace powercell {
namespace {
bool starts_key(const char* p, const char* key) {
  for (; *key; ++p, ++key) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != *key) return false;
  }
  return true;
}

const char* kKeys[] = {"password", "passwd", "api_key", "api-key", "authorization",
                       "bearer",   "token",  "secret",  nullptr};

void redact_value(char* p) {
  while (*p == ' ' || *p == '\t' || *p == '=' || *p == ':' || *p == '"' || *p == '\'') ++p;
  while (*p && *p != ' ' && *p != '\t' && *p != '"' && *p != '\'' && *p != ',' && *p != '}') {
    *p++ = 'x';
  }
}
}  // namespace

int redact_text(char* text, size_t cap) {
  (void)cap;
  if (!text) return 0;
  int hits = 0;
  for (char* p = text; *p;) {
    bool matched = false;
    for (int k = 0; kKeys[k]; ++k) {
      if (starts_key(p, kKeys[k])) {
        p += strlen(kKeys[k]);
        redact_value(p);
        ++hits;
        matched = true;
        break;
      }
    }
    if (!matched) ++p;
  }
  return hits;
}

DiagQueue::DiagQueue(uint32_t capacity)
    : items_(capacity ? static_cast<SlotRecord*>(calloc(capacity, sizeof(SlotRecord))) : nullptr),
      cap_(capacity) {}

DiagQueue::~DiagQueue() { free(items_); }

bool DiagQueue::try_push(const SlotRecord& record) {
  if (depth_) {
    ++dropped_;
    return false;
  }
  depth_++;
  bool ok = false;
  if (items_ && size_ < cap_) {
    items_[(head_ + size_) % cap_] = record;
    ++size_;
    ok = true;
  } else {
    ++dropped_;
  }
  depth_--;
  return ok;
}

bool DiagQueue::pop(SlotRecord* out) {
  if (!out || size_ == 0 || !items_) return false;
  *out = items_[head_];
  head_ = (head_ + 1) % cap_;
  --size_;
  return true;
}

ConsoleCapture::ConsoleCapture(Sink sink, void* ctx) : sink_(sink), ctx_(ctx) {}

void ConsoleCapture::byte(char c) {
  if (depth_) {
    ++dropped_;
    return;
  }
  if (c != '\n' && c != '\r') {
    if (len_ + 1 < static_cast<int>(sizeof(line_))) line_[len_++] = c;
    else truncated_ = true;
    return;
  }
  if (c == '\r') return;
  line_[len_] = 0;
  depth_++;
  if (sink_) sink_(line_, truncated_, ctx_);
  depth_--;
  len_ = 0;
  truncated_ = false;
}

bool UploadCursor::plan(const uint32_t* seqs, uint32_t count, uint32_t now_ms, uint16_t max_n,
                        uint32_t* first, uint32_t* last, uint16_t* n) {
  if (!contract_ || !seqs || !first || !last || !n || max_n == 0) return false;
  if (now_ms < backoff_until_) return false;
  uint32_t lo = 0;
  uint32_t hi = 0;
  uint16_t got = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (seqs[i] <= acked_) continue;
    if (got == 0) lo = seqs[i];
    hi = seqs[i];
    ++got;
    if (got >= max_n) break;
  }
  if (got == 0) return false;
  *first = lo;
  *last = hi;
  *n = got;
  return true;
}

void UploadCursor::on_transport_success() {}

void UploadCursor::on_ack(uint32_t seq, bool durable_commit, uint32_t now_ms) {
  (void)now_ms;
  if (!durable_commit || seq < acked_) return;
  acked_ = seq;
  backoff_ms_ = 1000;
  backoff_until_ = 0;
}

void UploadCursor::on_failure(uint32_t now_ms) {
  backoff_until_ = now_ms + backoff_ms_;
  if (backoff_ms_ < 30000) backoff_ms_ *= 2;
}

int format_diag_text(const SlotRecord& record, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  char text[65];
  memset(text, 0, sizeof(text));
  const uint8_t n = record.len < 64 ? record.len : 64;
  memcpy(text, record.payload, n);
  return snprintf(out, cap,
                  "seq=%lu mono_ms=%lu recv_unix=%lld clock=%u kind=%u flags=%u text=%s",
                  static_cast<unsigned long>(record.seq), static_cast<unsigned long>(record.mono_ms),
                  static_cast<long long>(record.recv_unix), record.clock_quality, record.kind,
                  record.flags, text);
}

SlotStatus route_diag(HardwareProfile profile, SdLog* sd, SlotLog* flash, const SlotRecord& record) {
  if (!diag_kind_allowed(profile, record.kind)) return SlotStatus::Invalid;
  SlotRecord copy = record;
  if (copy.kind == KindDiagConsole || copy.kind == KindDiagStm32 || copy.kind == KindDiagStm32Bad) {
    char text[65];
    memset(text, 0, sizeof(text));
    const uint8_t n = copy.len < 64 ? copy.len : 64;
    memcpy(text, copy.payload, n);
    redact_text(text, sizeof(text));
    memcpy(copy.payload, text, 64);
    copy.len = static_cast<uint8_t>(strlen(text) > 64 ? 64 : strlen(text));
  }
  const bool fault = slot_is_fault_mirror(copy.kind);
  if (sd) {
    const SdStatus status = sd->append(copy);
    if (status == SdStatus::Ok) {
      if (fault) sd->flush();
      if (fault && flash) flash->append(copy);
      return SlotStatus::Ok;
    }
  }
  if (flash) {
    const SlotStatus status = flash->append(copy);
    return status == SlotStatus::Ok ? SlotStatus::Ok : status;
  }
  return SlotStatus::IoError;
}

}  // namespace powercell
