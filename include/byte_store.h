#pragma once

#include <stdint.h>

namespace powercell {

// NOR-style image: a write may only clear bits. erase() restores 0xFF.
// flush() is a no-op for RAM and a barrier for buffered backends.
class ByteStore {
 public:
  virtual ~ByteStore() {}
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, uint8_t* dst, uint32_t len) const = 0;
  virtual bool write(uint32_t offset, const uint8_t* src, uint32_t len) = 0;
  virtual bool erase(uint32_t offset, uint32_t len) = 0;
  virtual bool flush() { return true; }
};

class MemoryByteStore : public ByteStore {
 public:
  explicit MemoryByteStore(uint32_t size);
  ~MemoryByteStore() override;
  uint32_t size() const override;
  bool read(uint32_t offset, uint8_t* dst, uint32_t len) const override;
  bool write(uint32_t offset, const uint8_t* src, uint32_t len) override;
  bool erase(uint32_t offset, uint32_t len) override;
  void fail_writes(bool fail);
  void tear_next(uint32_t bytes);
  uint32_t erase_count() const;
  uint32_t bytes_programmed() const;

 private:
  uint8_t* data_;
  uint32_t size_;
  bool fail_ = false;
  uint32_t tear_ = 0;
  uint32_t erases_ = 0;
  uint32_t programmed_ = 0;
};

}  // namespace powercell
