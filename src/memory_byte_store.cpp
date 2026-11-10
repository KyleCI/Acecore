#include "byte_store.h"

#include <stdlib.h>
#include <string.h>

namespace powercell {

MemoryByteStore::MemoryByteStore(uint32_t size)
    : data_(static_cast<uint8_t*>(malloc(size))), size_(size) {
  if (data_) memset(data_, 0xFF, size_);
}

MemoryByteStore::~MemoryByteStore() { free(data_); }

uint32_t MemoryByteStore::size() const { return data_ ? size_ : 0; }

bool MemoryByteStore::read(uint32_t offset, uint8_t* dst, uint32_t len) const {
  if (!data_ || !dst || offset + len < offset || offset + len > size_) return false;
  memcpy(dst, data_ + offset, len);
  return true;
}

bool MemoryByteStore::write(uint32_t offset, const uint8_t* src, uint32_t len) {
  if (!data_ || !src || offset + len < offset || offset + len > size_) return false;
  if (fail_) return false;
  uint32_t n = len;
  bool torn = false;
  if (tear_ > 0) {
    n = tear_ < len ? tear_ : len;
    tear_ = 0;
    torn = n < len;
  }
  for (uint32_t i = 0; i < n; ++i) {
    if ((data_[offset + i] & src[i]) != src[i]) return false;
    data_[offset + i] = static_cast<uint8_t>(data_[offset + i] & src[i]);
  }
  programmed_ += n;
  return !torn;
}

bool MemoryByteStore::erase(uint32_t offset, uint32_t len) {
  if (!data_ || offset + len < offset || offset + len > size_) return false;
  if (fail_) return false;
  memset(data_ + offset, 0xFF, len);
  ++erases_;
  return true;
}

void MemoryByteStore::fail_writes(bool fail) { fail_ = fail; }

void MemoryByteStore::tear_next(uint32_t bytes) { tear_ = bytes == 0 ? 1 : bytes; }

uint32_t MemoryByteStore::erase_count() const { return erases_; }

uint32_t MemoryByteStore::bytes_programmed() const { return programmed_; }

}  // namespace powercell
