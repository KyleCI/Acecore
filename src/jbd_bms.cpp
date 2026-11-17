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

bool checkFrame(const uint8_t* frame, size_t len, uint8_t expectReg, const uint8_t** data, uint8_t* dataLen) {
  if (!frame || !data || !dataLen || len < 7) return false;
  if (frame[0] != kStart || frame[len - 1] != kEnd) return false;
  if (frame[1] != expectReg) return false;
  if (frame[2] != 0x00) return false;
  const uint8_t n = frame[3];
  if (static_cast<size_t>(n) + 7 != len) return false;
  unsigned sum = static_cast<unsigned>(frame[2]) + frame[3];
  for (uint8_t i = 0; i < n; ++i) sum += frame[4 + i];
  const uint16_t chk = static_cast<uint16_t>(0x10000u - (sum & 0xFFFFu));
  if (frame[4 + n] != static_cast<uint8_t>(chk >> 8) || frame[5 + n] != static_cast<uint8_t>(chk & 0xFF)) return false;
  *data = frame + 4;
  *dataLen = n;
  return true;
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
  const uint8_t ntc = data[22];
  for (uint8_t i = 0; i < ntc && i < 2; ++i) {
    if (static_cast<unsigned>(23 + 2 * i + 1) >= len) break;
    const float c = (be16u(data + 23 + 2 * i) - 2731) * 0.1f;
    if (i == 0) { r.tempBat1 = c; r.hasTemp1 = true; }
    else { r.tempBat2 = c; r.hasTemp2 = true; }
  }
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
