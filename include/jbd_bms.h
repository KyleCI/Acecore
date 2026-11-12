#pragma once

#include <stddef.h>
#include <stdint.h>

// JBD / Xiaoxiang UART BMS (JBD-DB24SA01 style), 9600 8N1.
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
constexpr float kStatusDeadbandA = 0.2f;

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
  uint8_t soc = 0;
  uint8_t soh = 0;
  uint8_t batStatus = 0;
  float remainAh = 0;
  float fullAh = 0;
  uint32_t cycles = 0;
  bool chargeMos = false;
  bool dischargeMos = false;
  bool balancing = false;
  uint32_t alarmBits = 0;
  bool valid = false;
};

void buildRead(uint8_t reg, uint8_t out[7]);
// True when start, end, register, status 0, length and checksum all match.
bool checkFrame(const uint8_t* frame, size_t len, uint8_t expectReg, const uint8_t** data, uint8_t* dataLen);
bool parseBasic(const uint8_t* data, uint8_t len, Reading* out);
bool parseCells(const uint8_t* data, uint8_t len, Reading* out);
// Wrap a payload the way the BMS does, for tests and the checker.
size_t wrapResponse(uint8_t reg, uint8_t status, const uint8_t* data, uint8_t len, uint8_t* out, size_t cap);

}  // namespace jbd
}  // namespace powercell
