#include "serial_spool.h"

#include "diag_blackbox.h"

#include <stdio.h>
#include <string.h>

namespace powercell {
namespace {
void copy_fit(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  size_t i = 0;
  if (src) {
    for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
  }
  dst[i] = 0;
}

bool contains_token(const char* s, const char* token) {
  if (!s || !token) return false;
  const size_t n = strlen(token);
  for (const char* p = s; *p; ++p) {
    size_t i = 0;
    for (; i < n; ++i) {
      char c = p[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != token[i]) break;
    }
    if (i == n) return true;
  }
  return false;
}
}  // namespace

SerialSpool& console_spool() {
  static SerialSpool spool;
  return spool;
}

void SerialSpool::set_suppressed(bool suppressed) { suppressed_ = suppressed; }

bool SerialSpool::enqueue(const char* text) {
  if (qn_ >= kQueue) {
    dropped_bytes_ += text ? static_cast<uint32_t>(strlen(text)) : 0;
    return false;
  }
  const uint8_t slot = static_cast<uint8_t>((qh_ + qn_) % kQueue);
  copy_fit(queue_[slot], 64, text);
  ++qn_;
  copy_fit(recent_[recent_pos_], 64, text);
  recent_pos_ = static_cast<uint8_t>((recent_pos_ + 1) % kRecent);
  if (recent_n_ < kRecent) ++recent_n_;
  return true;
}

void SerialSpool::commit_line(bool force_truncate) {
  if (assem_len_ == 0) return;
  assem_[assem_len_] = 0;
  char line[sizeof(assem_)];
  memcpy(line, assem_, assem_len_ + 1);
  if (contains_token(line, "ota_config")) {
    copy_fit(line, sizeof line, "OTA_CONFIG [redacted]");
  } else {
    redact_text(line, sizeof line);
  }
  const size_t len = strlen(line);
  size_t chunks = (len + kChunkText - 1) / kChunkText;
  if (chunks == 0) chunks = 1;
  bool truncated = force_truncate;
  if (chunks > static_cast<size_t>(kMaxChunks)) {
    chunks = kMaxChunks;
    truncated = true;
    if (len > static_cast<size_t>(kChunkText * kMaxChunks)) {
      dropped_bytes_ += static_cast<uint32_t>(len - kChunkText * kMaxChunks);
    }
  }
  for (size_t i = 0; i < chunks; ++i) {
    const size_t off = i * kChunkText;
    const size_t n = off >= len ? 0 : (len - off > static_cast<size_t>(kChunkText)
                                           ? static_cast<size_t>(kChunkText)
                                           : len - off);
    char piece[64];
    snprintf(piece, sizeof piece, "[%u/%u%s] %.*s", static_cast<unsigned>(i + 1),
             static_cast<unsigned>(chunks), (truncated && i + 1 == chunks) ? " truncated" : "",
             static_cast<int>(n), line + off);
    if (!enqueue(piece) && dropped_bytes_ == 0) dropped_bytes_ = 1;
  }
  assem_len_ = 0;
}

void SerialSpool::ingest(const char* data, size_t n) {
  if (suppressed_ || !data || n == 0) return;
  for (size_t i = 0; i < n; ++i) {
    const char c = data[i];
    if (c == '\r') continue;
    if (c == '\n') {
      commit_line(false);
      continue;
    }
    if (assem_len_ >= kChunkText * kMaxChunks) {
      commit_line(true);
    }
    if (assem_len_ < sizeof(assem_) - 1) assem_[assem_len_++] = c;
  }
}

uint32_t SerialSpool::drain(SdLog* sd, int max_records) {
  if (!sd || max_records <= 0) return 0;
  uint32_t wrote = 0;
  while (wrote < static_cast<uint32_t>(max_records) && qn_ > 0) {
    SlotRecord rec;
    rec.kind = KindDiagConsole;
    const char* text = queue_[qh_];
    rec.len = static_cast<uint8_t>(strlen(text) > 63 ? 63 : strlen(text));
    memcpy(rec.payload, text, rec.len);
    if (strstr(text, "truncated")) rec.flags = FlagTruncatedText;
    if (sd->append(rec) != SdStatus::Ok) break;
    qh_ = static_cast<uint8_t>((qh_ + 1) % kQueue);
    --qn_;
    ++wrote;
  }
  if (wrote && !sd->flush()) return wrote;
  return wrote;
}

int SerialSpool::export_recent(char* out, size_t cap) const {
  if (!out || cap == 0) return 0;
  size_t used = 0;
  out[0] = 0;
  const uint8_t start = recent_n_ == kRecent ? recent_pos_ : 0;
  for (uint8_t i = 0; i < recent_n_ && used + 1 < cap; ++i) {
    const char* text = recent_[(start + i) % kRecent];
    const int n = snprintf(out + used, cap - used, "%s\n", text);
    if (n < 0 || static_cast<size_t>(n) >= cap - used) break;
    used += static_cast<size_t>(n);
  }
  if (dropped_bytes_ && used + 1 < cap) {
    const int n = snprintf(out + used, cap - used, "[serial dropped %lu bytes]\n",
                           static_cast<unsigned long>(dropped_bytes_));
    if (n > 0 && static_cast<size_t>(n) < cap - used) used += static_cast<size_t>(n);
  }
  return static_cast<int>(used);
}

int SerialSpool::export_sd(SdLog* sd, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = 0;
  if (!sd || !sd->stats().ready) {
    const int n = snprintf(out, cap, "sd_unavailable\n");
    size_t used = n > 0 ? static_cast<size_t>(n) : 0;
    if (used < cap) used += static_cast<size_t>(export_recent(out + used, cap - used));
    return static_cast<int>(used);
  }
  SlotRecord recs[16];
  const uint32_t n = sd->collect(KindDiagConsole, recs, 16);
  size_t used = 0;
  for (uint32_t i = 0; i < n && used + 1 < cap; ++i) {
    char text[65];
    memset(text, 0, sizeof text);
    const uint8_t len = recs[i].len < 64 ? recs[i].len : 64;
    memcpy(text, recs[i].payload, len);
    const int wrote = snprintf(out + used, cap - used, "%s\n", text);
    if (wrote < 0 || static_cast<size_t>(wrote) >= cap - used) break;
    used += static_cast<size_t>(wrote);
  }
  if (dropped_bytes_ && used + 1 < cap) {
    const int wrote = snprintf(out + used, cap - used, "[serial dropped %lu bytes]\n",
                               static_cast<unsigned long>(dropped_bytes_));
    if (wrote > 0 && static_cast<size_t>(wrote) < cap - used) used += static_cast<size_t>(wrote);
  }
  return static_cast<int>(used);
}

}  // namespace powercell
