#include "pzem_v3.h"

namespace powercell {

bool pzem_framing_supported(const PzemConfig& config) {
  return config.baud == 9600 && config.data_bits == 8 && config.parity == 0 &&
         config.stop_bits == 1 && config.address != 0;
}

uint16_t pzem_crc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
    }
  }
  return crc;
}

static size_t finish(const PzemConfig& config, uint8_t* frame, size_t n) {
  const uint16_t crc = pzem_crc(frame, n);
  if (config.crc_high_byte_first) {
    frame[n] = static_cast<uint8_t>(crc >> 8);
    frame[n + 1] = static_cast<uint8_t>(crc);
  } else {
    frame[n] = static_cast<uint8_t>(crc);
    frame[n + 1] = static_cast<uint8_t>(crc >> 8);
  }
  return n + 2;
}

size_t pzem_build_read(const PzemConfig& config, uint8_t* out, size_t cap) {
  if (!out || cap < 8 || !pzem_framing_supported(config)) return 0;
  out[0] = config.address;
  out[1] = 0x04;
  out[2] = 0x00;
  out[3] = 0x00;
  out[4] = 0x00;
  out[5] = 0x0A;
  return finish(config, out, 6);
}

size_t pzem_build_reset(const PzemConfig& config, uint8_t* out, size_t cap) {
  if (!out || cap < 4 || !pzem_framing_supported(config)) return 0;
  out[0] = config.address;
  out[1] = 0x42;
  return finish(config, out, 2);
}

static uint32_t word(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 8) | p[1];
}

static uint32_t reg32(const uint8_t* low_then_high) {
  return word(low_then_high) | (word(low_then_high + 2) << 16);
}

PzemReading pzem_parse(const PzemConfig& config, const uint8_t* frame, size_t len, bool timed_out) {
  PzemReading out;
  if (!pzem_framing_supported(config)) {
    out.status = PzemStatus::UnsupportedFraming;
    return out;
  }
  if (timed_out && len < 25) {
    out.status = PzemStatus::Timeout;
    return out;
  }
  if (!frame || len < 5) {
    out.status = PzemStatus::Short;
    return out;
  }
  if (frame[0] != config.address) {
    out.status = PzemStatus::BadAddress;
    return out;
  }
  const uint16_t crc = pzem_crc(frame, len - 2);
  const uint16_t got = config.crc_high_byte_first
                           ? static_cast<uint16_t>((frame[len - 2] << 8) | frame[len - 1])
                           : static_cast<uint16_t>(frame[len - 2] | (frame[len - 1] << 8));
  if (got != crc) {
    out.status = PzemStatus::BadCrc;
    return out;
  }
  if (frame[1] & 0x80) {
    out.status = PzemStatus::Exception;
    return out;
  }
  if (frame[1] != 0x04 || len < 25 || frame[2] != 20) {
    out.status = PzemStatus::Short;
    return out;
  }
  const uint8_t* d = frame + 3;
  out.voltage_dV = word(d);
  out.current_mA = reg32(d + 2);
  out.power_dW = reg32(d + 6);
  out.energy_Wh = reg32(d + 10);
  out.frequency_dHz = word(d + 14);
  out.power_factor_c = word(d + 16);
  out.alarm = static_cast<uint16_t>(word(d + 18));
  out.valid = true;
  out.status = PzemStatus::Ok;
  return out;
}

bool pzem_wh(const PzemReading& reading, uint32_t* wh) {
  if (!reading.valid || reading.status != PzemStatus::Ok || !wh) return false;
  *wh = reading.energy_Wh;
  return true;
}

}  // namespace powercell
