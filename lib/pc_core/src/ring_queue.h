#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pc {
// Fixed-capacity FIFO. When full, push() overwrites the oldest entry and
// counts a drop, so memory never grows and the newest events survive.
template <typename T, size_t N>
class RingQueue {
 public:
  bool push(const T &v) {
    bool dropped = false;
    if (count_ == N) { head_ = (head_ + 1) % N; --count_; ++dropped_; dropped = true; }
    buf_[(head_ + count_) % N] = v;
    ++count_;
    return !dropped;
  }
  // i = 0 is the oldest entry.
  const T *peek(size_t i = 0) const { return i < count_ ? &buf_[(head_ + i) % N] : nullptr; }
  T *at(size_t i) { return i < count_ ? &buf_[(head_ + i) % N] : nullptr; }
  bool pop(T *out = nullptr) {
    if (!count_) return false;
    if (out) *out = buf_[head_];
    head_ = (head_ + 1) % N; --count_;
    return true;
  }
  // Drop the n oldest entries (after a batch was acknowledged).
  size_t popN(size_t n) { size_t k = n < count_ ? n : count_; head_ = (head_ + k) % N; count_ -= k; return k; }
  // Remove every entry for which pred(entry) is true, keeping order.
  template <typename P>
  size_t removeIf(P pred) {
    size_t w = 0, removed = 0;
    for (size_t r = 0; r < count_; ++r) {
      T &e = buf_[(head_ + r) % N];
      if (pred(e)) { ++removed; continue; }
      if (w != r) buf_[(head_ + w) % N] = e;
      ++w;
    }
    count_ = w;
    return removed;
  }
  void clear() { head_ = count_ = 0; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  static constexpr size_t capacity() { return N; }
  uint32_t dropped() const { return dropped_; }

 private:
  T buf_[N];
  size_t head_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};
}  // namespace pc
