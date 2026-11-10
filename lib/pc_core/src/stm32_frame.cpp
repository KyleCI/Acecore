#include "stm32_frame.h"
#include <string.h>
#include "crc.h"

namespace pc {
namespace frame {

size_t encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len, uint8_t *out, size_t outCap) {
  if (len > kMaxPayload || outCap < len + kOverhead || (len && !payload)) return 0;
  out[0] = kSync;
  out[1] = static_cast<uint8_t>(len);
  out[2] = kVersion;
  out[3] = type;
  out[4] = seq;
  if (len) memcpy(out + 5, payload, len);
  uint16_t crc = crc16_ccitt(out + 1, 4 + len);
  out[5 + len] = crc & 0xFF;
  out[6 + len] = crc >> 8;
  return len + kOverhead;
}

bool Decoder::feed(uint8_t b, uint32_t now, Frame &f) {
  if (state_ != 0 && (uint32_t)(now - lastMs_) > kInterByteTimeoutMs) { ++timeouts_; state_ = 0; }
  lastMs_ = now;
  switch (state_) {
    case 0:
      if (b == kSync) state_ = 1;
      return false;
    case 1: work_.len = b; state_ = 2; return false;
    case 2:
      work_.version = b;
      if (b != kVersion) { ++versionErrors_; state_ = (b == kSync) ? 1 : 0; return false; }
      state_ = 3; return false;
    case 3: work_.type = b; state_ = 4; return false;
    case 4: work_.seq = b; idx_ = 0; state_ = work_.len ? 5 : 6; return false;
    case 5:
      work_.payload[idx_++] = b;
      if (idx_ >= work_.len) state_ = 6;
      return false;
    case 6: crcRx_ = b; state_ = 7; return false;
    case 7: {
      crcRx_ |= static_cast<uint16_t>(b) << 8;
      state_ = 0;
      uint8_t hdr[4] = {work_.len, work_.version, work_.type, work_.seq};
      uint16_t crc = crc16_ccitt(hdr, 4);
      crc = crc16_ccitt(work_.payload, work_.len, crc);
      if (crc != crcRx_) { ++crcErrors_; return false; }
      f = work_;
      return true;
    }
  }
  state_ = 0;
  return false;
}

bool decodeStatus(const uint8_t *p, size_t len, Stm32Status &o) {
  if (!p || len < kStatusLen) return false;
  Stm32Status s;
  s.sys = p[0] <= 5 ? static_cast<SysState>(p[0]) : SysState::Unknown;
  s.sw = p[1] <= 2 ? static_cast<SwitchPos>(p[1]) : SwitchPos::Unknown;
  s.chr = p[2] <= 7 ? static_cast<ChargeState>(p[2]) : ChargeState::Unknown;
  s.has_ext = true;
  s.restart_count = p[3];
  s.fault = getU32(p + 4);
  s.has_rsv = true; s.rsv_latched = (s.fault & (1u << 20)) || s.sys == SysState::Reserve;  // reserve latch hook: status bit, not a fault
  uint16_t flags = getU16(p + 28);
  s.has_bat = flags & VALID_BAT;
  s.bat_v = getU16(p + 8) / 1000.0f;
  s.has_bat_pct = p[10] != 0xFF && p[10] <= 100;
  s.bat_pct = s.has_bat_pct ? p[10] : 0;
  s.transfer_flags = p[11];
  s.has_grid = flags & VALID_GRID;
  s.grid_v = getU16(p + 12) / 10.0f;
  s.has_temp = flags & VALID_TEMP;
  s.temp_c = static_cast<int16_t>(getU16(p + 14)) / 10.0f;
  s.has_vout = s.has_iout = flags & VALID_OUTPUT;
  s.vout = getU16(p + 16) / 10.0f;
  s.iout = getU16(p + 18) / 100.0f;
  s.chg_current = getU16(p + 20) / 100.0f;
  s.mod_index = getU16(p + 22) / 10000.0f;
  s.uptime_s = getU32(p + 24);
  o = s;
  return true;
}

static uint16_t clampU16(float v) { return v <= 0 ? 0 : v >= 65535.0f ? 65535 : (uint16_t)(v + 0.5f); }

size_t encodeStatus(const Stm32Status &s, uint32_t uptime, uint8_t *p, size_t cap) {
  if (!p || cap < kStatusLen) return 0;
  p[0] = (uint8_t)s.sys; p[1] = (uint8_t)s.sw; p[2] = (uint8_t)s.chr; p[3] = s.restart_count;
  putU32(p + 4, s.fault);
  putU16(p + 8, clampU16(s.bat_v * 1000));
  p[10] = s.has_bat_pct ? s.bat_pct : 0xFF;
  p[11] = s.transfer_flags;
  putU16(p + 12, clampU16(s.grid_v * 10));
  float t = s.temp_c * 10; t = t < -32768 ? -32768 : t > 32767 ? 32767 : t;
  putU16(p + 14, (uint16_t)(int16_t)(t < 0 ? t - 0.5f : t + 0.5f));
  putU16(p + 16, clampU16(s.vout * 10));
  putU16(p + 18, clampU16(s.iout * 100));
  putU16(p + 20, clampU16(s.chg_current * 100));
  putU16(p + 22, clampU16(s.mod_index * 10000));
  putU32(p + 24, uptime);
  putU16(p + 28, (s.has_bat ? VALID_BAT : 0) | (s.has_grid ? VALID_GRID : 0) | (s.has_temp ? VALID_TEMP : 0) |
                     (s.has_vout ? VALID_OUTPUT : 0));
  return kStatusLen;
}
}  // namespace frame
}  // namespace pc
