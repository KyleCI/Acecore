#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jk_bms.h"

// JBD / Xiaoxiang UART BMS (JBD-DB24SA01 / SP-series style), 9600 8N1. 2.5.35 product BMS.
// Request:  DD A5 <reg> 00 <chk hi> <chk lo> 77
// Response: DD <reg> <status> <len> <data...> <chk hi> <chk lo> 77
// Checksum: 0x10000 - (status + len + sum(data)); request uses reg + len.
// Registers: 0x03 basic info, 0x04 cell voltages.
// Current sign matches the JK path: positive charges, negative discharges.
namespace powercell {
namespace jbd {

constexpr uint8_t kStart = 0xDD;
constexpr uint8_t kEnd = 0x77;
constexpr uint8_t kRead = 0xA5;
constexpr uint8_t kRegBasic = 0x03;
constexpr uint8_t kRegCells = 0x04;
constexpr uint8_t kMaxCells = 32;
constexpr uint8_t kMaxNtc = 8;
constexpr float kStatusDeadbandA = 0.2f;

// Protection word (0x03 offsets 16..17), JBD bit -> meaning:
//  0 cell OV, 1 cell UV, 2 pack OV, 3 pack UV, 4 charge OT, 5 charge UT,
//  6 discharge OT, 7 discharge UT, 8 charge OC, 9 discharge OC, 10 short circuit,
//  11 front-end IC error, 12 MOS software lock. Bits 13..15 are reserved.
constexpr uint16_t kProtCellOv = 1u << 0, kProtCellUv = 1u << 1, kProtPackOv = 1u << 2, kProtPackUv = 1u << 3,
                   kProtChgOt = 1u << 4, kProtChgUt = 1u << 5, kProtDsgOt = 1u << 6, kProtDsgUt = 1u << 7,
                   kProtChgOc = 1u << 8, kProtDsgOc = 1u << 9, kProtShort = 1u << 10, kProtAfe = 1u << 11,
                   kProtMosLock = 1u << 12;

struct Reading {
  float cellVoltage[kMaxCells] = {};
  uint8_t cellCount = 0;
  float cellMaxV = 0;
  float cellMinV = 0;
  float cellDiffMv = 0;
  float avgCellV = 0;
  uint8_t maxCell = 0;
  uint8_t minCell = 0;
  float packVoltage = 0;
  float packCurrent = 0;
  float packPower = 0;
  float tempBat1 = 0;
  float tempBat2 = 0;
  bool hasTemp1 = false;
  bool hasTemp2 = false;
  uint8_t ntcCount = 0;            // NTCs parsed into temps[]
  float temps[kMaxNtc] = {};
  uint8_t soc = 0;
  uint8_t soh = 0;
  uint8_t batStatus = 0;
  float remainAh = 0;
  float fullAh = 0;
  uint32_t cycles = 0;
  bool chargeMos = false;
  bool dischargeMos = false;
  bool balancing = false;
  uint32_t alarmBits = 0;          // raw JBD protection word
  bool hasCells = false;           // a 0x04 reply was parsed
  bool valid = false;
};

enum class Frame : uint8_t { Ok, BadFrame, ChecksumError, StatusError };
const char* frameName(Frame f);

void buildRead(uint8_t reg, uint8_t out[7]);
// True when start, end, register, status 0, length and checksum all match.
bool checkFrame(const uint8_t* frame, size_t len, uint8_t expectReg, const uint8_t** data, uint8_t* dataLen);
// Same checks, with the reason a frame was refused (for the BMS stats counters).
Frame classifyFrame(const uint8_t* frame, size_t len, uint8_t expectReg, const uint8_t** data, uint8_t* dataLen);
// JBD protection word -> JK-PB alarm bit layout, so events (bms_N ids, bms_over_temp,
// overcurrent, battery_low, bms_fault) and telemetry names are shared with the JK path.
uint32_t toJkAlarms(uint16_t protection);
// Reading -> the shared pc::jk::Data the rest of the firmware reads. When the reading has
// no cells (0x04 failed) the cells of `prevCells` are kept if it has the same count, else
// cellCount is 0. NTC1/NTC2 -> tempC[0]/[1]; JBD has no MOS sensor or SOH (hasMosTemp/hasSoh false).
void toJkData(const Reading& r, const pc::jk::Data* prevCells, pc::jk::Data* out);
bool parseBasic(const uint8_t* data, uint8_t len, Reading* out);
bool parseCells(const uint8_t* data, uint8_t len, Reading* out);
// Wrap a payload the way the BMS does, for tests and the checker.
size_t wrapResponse(uint8_t reg, uint8_t status, const uint8_t* data, uint8_t len, uint8_t* out, size_t cap);

}  // namespace jbd
}  // namespace powercell
