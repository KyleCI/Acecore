#pragma once
#include <stddef.h>
#include <stdint.h>

// JK-PB series BMS frames (host-testable, no Arduino).
// The BMS streams fixed 300-byte frames on its RS485 port; the ESP32 listens
// on UART2 (IO16 RX / IO17 TX, 115200 8N1) through an auto-direction
// TTL-RS485 module. Ported and cleaned up from the Dubem collector firmware
// (src/bms.cpp, include/bms.h), which is the hardware team's reference.
//
// Frame: 55 AA EB 90 | type | counter | data ... | checksum (byte 299 = sum of bytes 0..298, low 8 bits)
//   type 0x02 = DYNAMIC (cells, pack, temps, SOC, alarms), parsed here
//   type 0x01 = SETUP (settings), recognised and skipped
// Offsets and the checksum follow the public JK-PB/JK02 32S layout as used by
// github.com/syssi/esphome-jk-bms (jk_bms_ble / jk_rs485_bms) and match the
// Dubem parser. All multi-byte values are little-endian.
namespace pc {
namespace jk {

constexpr size_t kFrameLen = 300;
constexpr uint8_t kTypeSetup = 0x01, kTypeDynamic = 0x02;
constexpr int kMaxCells = 32;
constexpr int kAlarmBits = 24;
const char *alarmName(int bit);  // JK alarm bit 0..23, nullptr if unused

struct Data {
  uint8_t cellCount = 0;
  uint16_t cellMv[kMaxCells] = {};
  uint16_t cellAvgMv = 0, cellDiffMv = 0;
  uint8_t cellMax = 0, cellMin = 0;      // 1-based cell numbers
  uint32_t packMv = 0;
  int32_t currentMa = 0;                 // + charging, - discharging
  int32_t powerMw = 0;
  float mosTempC = 0, tempC[2] = {};
  uint32_t alarms = 0;                   // raw JK alarm bits
  uint8_t status = 0;                    // 0 standby, 1 charging, 2 discharging (as sent)
  uint8_t soc = 0, soh = 0;
  int32_t remainMah = 0;
  uint32_t fullMah = 0, cycles = 0;
  bool chargeMos = false, dischargeMos = false, balancing = false;
};

enum class Parse : uint8_t { Ok, NotDynamic, ChecksumError, BadFrame };
const char *parseName(Parse p);

uint8_t frameChecksum(const uint8_t *f);  // sum of bytes 0..298
// Validate and decode one 300-byte frame. `out` is untouched unless Ok.
Parse parseDynamic(const uint8_t *f, size_t len, Data &out);

// Byte-stream assembler: finds the 55 AA EB 90 header and collects 300 bytes.
// A gap longer than kGapMs inside a frame drops it (the BMS sends a frame in ~26 ms).
class Assembler {
 public:
  static constexpr uint32_t kGapMs = 100;
  // Returns true when buf() holds a complete 300-byte frame.
  bool feed(uint8_t b, uint32_t nowMs);
  const uint8_t *buf() const { return buf_; }
  uint32_t timeouts() const { return timeouts_; }
 private:
  uint8_t buf_[kFrameLen];
  size_t n_ = 0;
  uint32_t last_ = 0, timeouts_ = 0;
};

}  // namespace jk
}  // namespace pc
