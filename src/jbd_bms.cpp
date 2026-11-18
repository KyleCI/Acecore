#include "jbd_bms.h"

#include <math.h>
#include <string.h>

namespace powercell {
namespace jbd {
namespace {
uint16_t be16u(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
int16_t be16s(const uint8_t* p) { return static_cast<int16_t>((p[0] << 8) | p[1]); }
}  // namespace

void buildRead(uint8_t reg, uint8_t out[7]) {
  out[0] = kStart;
  out[1] = kRead;
  out[2] = reg;
  out[3] = 0x00;
  const uint16_t chk = static_cast<uint16_t>(0x10000u - (static_cast<unsigned>(reg) + 0u));
  out[4] = static_cast<uint8_t>(chk >> 8);
  out[5] = static_cast<uint8_t>(chk & 0xFF);
  out[6] = kEnd;
}

const char* frameName(Frame f) {
  switch (f) {
    case Frame::Ok: return "ok";
    case Frame::BadFrame: return "bad_frame";
    case Frame::ChecksumError: return "checksum";
    case Frame::StatusError: return "status_error";
  }
  return "?";
}

Frame classifyFrame(const uint8_t* frame, size_t len, uint8_t expectReg, const uint8_t** data, uint8_t* dataLen) {
  if (!frame || !data || !dataLen || len < 7) return Frame::BadFrame;
  if (frame[0] != kStart || frame[len - 1] != kEnd) return Frame::BadFrame;
  if (frame[1] != expectReg) return Frame::BadFrame;
  const uint8_t n = frame[3];
  if (static_cast<size_t>(n) + 7 != len) return Frame::BadFrame;
  unsigned sum = static_cast<unsigned>(frame[2]) + frame[3];
  for (uint8_t i = 0; i < n; ++i) sum += frame[4 + i];
  const uint16_t chk = static_cast<uint16_t>(0x10000u - (sum & 0xFFFFu));
  if (frame[4 + n] != static_cast<uint8_t>(chk >> 8) || frame[5 + n] != static_cast<uint8_t>(chk & 0xFF))
    return Frame::ChecksumError;
  if (frame[2] != 0x00) return Frame::StatusError;  // 0x80 = BMS refused the read
  *data = frame + 4;
  *dataLen = n;
  return Frame::Ok;
}

bool checkFrame(const uint8_t* frame, size_t len, uint8_t expectReg, const uint8_t** data, uint8_t* dataLen) {
  return classifyFrame(frame, len, expectReg, data, dataLen) == Frame::Ok;
}

uint32_t toJkAlarms(uint16_t p) {
  uint32_t a = 0;
  auto map = [&](uint16_t bit, int jkBit) { if (p & bit) a |= 1u << jkBit; };
  map(kProtCellOv, 4);    // cell_overvoltage
  map(kProtCellUv, 11);   // cell_undervoltage
  map(kProtPackOv, 5);    // pack_overvoltage
  map(kProtPackUv, 12);   // pack_undervoltage
  map(kProtChgOt, 8);     // charge_overtemp    -> bms_over_temp
  map(kProtChgUt, 9);     // charge_undertemp
  map(kProtDsgOt, 15);    // discharge_overtemp -> bms_over_temp
  map(kProtDsgUt, pc::jk::kAlarmJbdDischargeUndertemp);
  map(kProtChgOc, 6);     // charge_overcurrent
  map(kProtDsgOc, 13);    // discharge_overcurrent
  map(kProtShort, 14);    // discharge_short_circuit
  map(kProtAfe, pc::jk::kAlarmJbdAfeError);
  map(kProtMosLock, pc::jk::kAlarmJbdMosLock);
  return a;  // bits 13..15 (reserved) are dropped
}

void toJkData(const Reading& r, const pc::jk::Data* prev, pc::jk::Data* out) {
  if (!out) return;
  pc::jk::Data d;
  if (r.hasCells) {
    d.cellCount = r.cellCount > pc::jk::kMaxCells ? pc::jk::kMaxCells : r.cellCount;
    for (uint8_t i = 0; i < d.cellCount; ++i) d.cellMv[i] = static_cast<uint16_t>(r.cellVoltage[i] * 1000.f + 0.5f);
    d.cellAvgMv = static_cast<uint16_t>(r.avgCellV * 1000.f + 0.5f);
    d.cellDiffMv = static_cast<uint16_t>(r.cellDiffMv + 0.5f);
    d.cellMax = static_cast<uint8_t>(r.maxCell + 1);
    d.cellMin = static_cast<uint8_t>(r.minCell + 1);
  } else if (prev && prev->cellCount && prev->cellCount == r.cellCount) {
    d.cellCount = prev->cellCount;
    memcpy(d.cellMv, prev->cellMv, sizeof d.cellMv);
    d.cellAvgMv = prev->cellAvgMv; d.cellDiffMv = prev->cellDiffMv;
    d.cellMax = prev->cellMax; d.cellMin = prev->cellMin;
  } else {
    d.cellCount = 0;  // never publish a 0 V cell list
  }
  d.packMv = static_cast<uint32_t>(r.packVoltage * 1000.f + 0.5f);
  d.currentMa = static_cast<int32_t>(lroundf(r.packCurrent * 1000.f));
  d.powerMw = static_cast<int32_t>(lroundf(r.packPower * 1000.f));
  d.tempCount = r.ntcCount > 2 ? 2 : r.ntcCount;
  if (d.tempCount > 0) d.tempC[0] = r.temps[0];
  if (d.tempCount > 1) d.tempC[1] = r.temps[1];
  d.hasMosTemp = false;
  d.mosTempC = 0;
  d.alarms = toJkAlarms(static_cast<uint16_t>(r.alarmBits));
  d.status = r.batStatus;
  d.soc = r.soc;
  d.hasSoh = false;
  d.soh = 0;
  d.remainMah = static_cast<int32_t>(lroundf(r.remainAh * 1000.f));
  d.fullMah = static_cast<uint32_t>(lroundf(r.fullAh * 1000.f));
  d.cycles = r.cycles;
  d.chargeMos = r.chargeMos;
  d.dischargeMos = r.dischargeMos;
  d.balancing = r.balancing;
  *out = d;
}

bool parseBasic(const uint8_t* data, uint8_t len, Reading* out) {
  if (!data || !out || len < 23) return false;
  Reading r;
  r.packVoltage = be16u(data + 0) * 0.01f;
  r.packCurrent = be16s(data + 2) * 0.01f;
  r.packPower = r.packVoltage * r.packCurrent;
  r.remainAh = be16u(data + 4) * 0.01f;
  r.fullAh = be16u(data + 6) * 0.01f;
  r.cycles = be16u(data + 8);
  r.balancing = (be16u(data + 12) | be16u(data + 14)) != 0;
  r.alarmBits = be16u(data + 16);
  r.soc = data[19];
  r.chargeMos = (data[20] & 0x01) != 0;
  r.dischargeMos = ((data[20] >> 1) & 0x01) != 0;
  r.cellCount = data[21] > kMaxCells ? kMaxCells : data[21];
  r.soh = 0;
  // NTCs: count at offset 22, then 2 bytes each in 0.1 K. Take only what the frame holds.
  const uint8_t ntc = data[22];
  for (uint8_t i = 0; i < ntc && i < kMaxNtc; ++i) {
    const unsigned off = 23u + 2u * i;
    if (off + 1 >= len) break;
    const float c = (static_cast<int>(be16u(data + off)) - 2731) * 0.1f;
    if (c < -40.f || c > 120.f) break;  // open or shorted NTC: stop, never publish it
    r.temps[r.ntcCount++] = c;
  }
  if (r.ntcCount > 0) { r.tempBat1 = r.temps[0]; r.hasTemp1 = true; }
  if (r.ntcCount > 1) { r.tempBat2 = r.temps[1]; r.hasTemp2 = true; }
  if (r.packCurrent > kStatusDeadbandA) r.batStatus = 1;
  else if (r.packCurrent < -kStatusDeadbandA) r.batStatus = 2;
  else r.batStatus = 0;
  // A SOC byte that disagrees with remaining/full capacity is not a measurement.
  // Same rule as the JK parser: it must not latch or release the battery cutoff.
  if (r.fullAh > 0.05f) {
    const float derived = r.remainAh <= 0 ? 0.f : (r.remainAh / r.fullAh) * 100.f;
    if (r.soc <= 10 && derived >= 15.f) return false;
    if (r.soc >= 15 && derived + 20.f < r.soc) return false;
  }
  r.valid = r.soc <= 100 && r.cellCount > 0 && r.packVoltage > 0.f;
  if (!r.valid) return false;
  *out = r;
  return true;
}

bool parseCells(const uint8_t* data, uint8_t len, Reading* out) {
  if (!data || !out || (len & 1)) return false;
  const uint8_t n = static_cast<uint8_t>(len / 2);
  if (n == 0 || n > kMaxCells) return false;
  float mn = 9.9f, mx = 0.f, sum = 0.f;
  uint8_t maxI = 0, minI = 0;
  for (uint8_t i = 0; i < n; ++i) {
    const float v = be16u(data + 2 * i) * 0.001f;
    out->cellVoltage[i] = v;
    sum += v;
    if (v > mx) { mx = v; maxI = i; }
    if (v < mn) { mn = v; minI = i; }
  }
  for (uint8_t i = n; i < kMaxCells; ++i) out->cellVoltage[i] = 0.f;
  out->cellCount = n;
  out->hasCells = true;
  out->cellMaxV = mx;
  out->cellMinV = mn;
  out->maxCell = maxI;
  out->minCell = minI;
  out->cellDiffMv = (mx - mn) * 1000.f;
  out->avgCellV = sum / n;
  return true;
}

size_t wrapResponse(uint8_t reg, uint8_t status, const uint8_t* data, uint8_t len, uint8_t* out, size_t cap) {
  if (!out || cap < static_cast<size_t>(len) + 7) return 0;
  out[0] = kStart;
  out[1] = reg;
  out[2] = status;
  out[3] = len;
  if (len && data) memcpy(out + 4, data, len);
  unsigned sum = static_cast<unsigned>(status) + len;
  for (uint8_t i = 0; i < len; ++i) sum += out[4 + i];
  const uint16_t chk = static_cast<uint16_t>(0x10000u - (sum & 0xFFFFu));
  out[4 + len] = static_cast<uint8_t>(chk >> 8);
  out[5 + len] = static_cast<uint8_t>(chk & 0xFF);
  out[6 + len] = kEnd;
  return static_cast<size_t>(len) + 7;
}

}  // namespace jbd
}  // namespace powercell
