#pragma once

#include <stddef.h>
#include <stdint.h>

// IEEE CRC-32 (reflected polynomial 0xEDB88320). Used for local records only.
inline uint32_t crc32_ieee(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return ~crc;
}
