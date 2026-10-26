#include "jk_bms.h"
#include <string.h>

namespace pc {
namespace jk {
namespace {
const uint8_t kHdr[4] = {0x55, 0xAA, 0xEB, 0x90};
uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// JK-PB alarm bits (JK "BMS RS485 Modbus V1.0" register 0x12A0, same word as the frame's offset 166).
const char *const kAlarms[kAlarmBits] = {
    "wire_resistance", "mos_overtemp", "cell_count", "current_sensor", "cell_overvoltage", "pack_overvoltage",
    "charge_overcurrent", "charge_short_circuit", "charge_overtemp", "charge_undertemp", "internal_comm",
    "cell_undervoltage", "pack_undervoltage", "discharge_overcurrent", "discharge_short_circuit",
    "discharge_overtemp", "charge_mos_fault", "discharge_mos_fault", "gps_disconnected", "password_change_due",
    "discharge_on_failed", "battery_overtemp", "temp_sensor_fault", "parallel_module_fault"};
}  // namespace

const char *alarmName(int bit) { return bit >= 0 && bit < kAlarmBits ? kAlarms[bit] : nullptr; }

const char *parseName(Parse p) {
  switch (p) {
    case Parse::Ok: return "ok";
    case Parse::NotDynamic: return "not_dynamic";
    case Parse::ChecksumError: return "checksum_error";
    default: return "bad_frame";
  }
}

uint8_t frameChecksum(const uint8_t *f) {
  uint8_t s = 0;
  for (size_t i = 0; i < kFrameLen - 1; ++i) s = uint8_t(s + f[i]);
  return s;
}

Parse parseDynamic(const uint8_t *f, size_t len, Data &out) {
  if (!f || len != kFrameLen || memcmp(f, kHdr, 4) != 0) return Parse::BadFrame;
  if (frameChecksum(f) != f[kFrameLen - 1]) return Parse::ChecksumError;
  if (f[4] != kTypeDynamic) return Parse::NotDynamic;
  Data r;
  uint16_t mx = 0, mn = 0xFFFF;
  for (int i = 0; i < kMaxCells; ++i) {
    uint16_t mv = le16(f + 6 + 2 * i);
    r.cellMv[i] = mv;
    if (mv > 500) {  // empty slots read 0
      r.cellCount = uint8_t(i + 1);
      if (mv > mx) mx = mv;
      if (mv < mn) mn = mv;
    }
  }
  r.cellAvgMv = le16(f + 74);
  r.cellDiffMv = r.cellCount ? uint16_t(mx - mn) : 0;
  r.cellMax = uint8_t(f[78] + 1);
  r.cellMin = uint8_t(f[79] + 1);
  r.mosTempC = int16_t(le16(f + 144)) / 10.0f;
  r.packMv = le32(f + 150);
  r.powerMw = int32_t(le32(f + 154));
  r.currentMa = int32_t(le32(f + 158));
  r.tempC[0] = int16_t(le16(f + 162)) / 10.0f;
  r.tempC[1] = int16_t(le16(f + 164)) / 10.0f;
  r.alarms = le32(f + 166);
  r.status = f[172];
  r.soc = f[173];
  r.remainMah = int32_t(le32(f + 174));
  r.fullMah = le32(f + 178);
  r.cycles = le32(f + 182);
  r.soh = f[190];
  r.chargeMos = f[198]; r.dischargeMos = f[199]; r.balancing = f[200];
  // A zero SOC is legitimate only in a plausible, complete frame.
  if (r.soc > 100 || !r.cellCount || r.packMv == 0) return Parse::BadFrame;
  out = r;
  return Parse::Ok;
}

bool Assembler::feed(uint8_t b, uint32_t now) {
  if (n_ && now - last_ > kGapMs) { ++timeouts_; n_ = 0; }
  last_ = now;
  if (n_ < 4) {
    if (b == kHdr[n_]) ++n_;
    else n_ = (b == kHdr[0]) ? 1 : 0;
    return false;
  }
  buf_[n_++] = b;
  if (n_ < kFrameLen) return false;
  memcpy(buf_, kHdr, 4);
  n_ = 0;
  return true;
}
}  // namespace jk
}  // namespace pc
