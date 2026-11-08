#pragma once

#include <stddef.h>
#include <stdint.h>

namespace powercell {

// PZEM-004T V3 Modbus-RTU, 9600 8N1, active energy in whole watt-hours.
// The manual's worked example is slave 0x01. Address 0xF8 is the documented
// single-slave general address. CRC on the wire is Modbus low-byte first;
// the manual's "CRC high byte then low byte" wording is selectable and off
// by default. This codec never resets the meter.

struct PzemConfig {
  uint8_t address = 0xF8;
  uint32_t baud = 9600;
  uint8_t data_bits = 8;
  uint8_t parity = 0;
  uint8_t stop_bits = 1;
  uint32_t timeout_ms = 200;
  bool crc_high_byte_first = false;
};

enum class PzemStatus : uint8_t {
  Ok = 0,
  Timeout,
  Short,
  BadCrc,
  BadAddress,
  Exception,
  UnsupportedFraming,
};

struct PzemReading {
  PzemStatus status = PzemStatus::Short;
  bool valid = false;
  uint32_t voltage_dV = 0;
  uint32_t current_mA = 0;
  uint32_t power_dW = 0;
  uint32_t energy_Wh = 0;
  uint32_t frequency_dHz = 0;
  uint32_t power_factor_c = 0;
  uint16_t alarm = 0;
};

inline bool pzem_automatic_reset() { return false; }

bool pzem_framing_supported(const PzemConfig& config);
uint16_t pzem_crc(const uint8_t* data, size_t len);
size_t pzem_build_read(const PzemConfig& config, uint8_t* out, size_t cap);
size_t pzem_build_reset(const PzemConfig& config, uint8_t* out, size_t cap);
PzemReading pzem_parse(const PzemConfig& config, const uint8_t* frame, size_t len, bool timed_out);
bool pzem_wh(const PzemReading& reading, uint32_t* wh);

}  // namespace powercell
