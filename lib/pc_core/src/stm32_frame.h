#pragma once
#include <stddef.h>
#include <stdint.h>
#include "stm32_types.h"

// Binary ESP32 <-> STM32 framing, agreed with the STM32 side (see
// /workspace/esp32_review/bootloader_protocol.md section 1):
//   0xAA | LEN | VER | TYPE | SEQ | PAYLOAD[LEN] | CRC16 (LE)
// CRC-16/CCITT-FALSE over LEN..PAYLOAD (sync byte excluded).
namespace pc {
namespace frame {
constexpr uint8_t kSync = 0xAA;
constexpr uint8_t kVersion = 0x01;
constexpr size_t kMaxPayload = 255;
constexpr size_t kOverhead = 7;  // sync, len, ver, type, seq, crc16
constexpr uint32_t kInterByteTimeoutMs = 20;
constexpr uint8_t kResponseBit = 0x80;

// Application message types (0x01-0x3F). Bootloader types are 0x40-0x5F.
enum Type : uint8_t {
  GET_STATUS = 0x01, SET_MODE = 0x02, FAULT_RESET = 0x03, GET_VERSION = 0x04,
  STATUS_PUSH = 0x10,          // STM32 -> ESP32, unsolicited, every 1 s
  ENTER_BOOTLOADER = 0x30,     // payload u32 magic 0xB007B007, only honoured in SYS_IDLE
  BL_HELLO = 0x40, BL_GET_INFO = 0x41, BL_ERASE = 0x42, BL_WRITE = 0x43, BL_VERIFY = 0x44,
  BL_SET_VALID = 0x45, BL_BOOT = 0x46, BL_GET_PROGRESS = 0x47, BL_ABORT = 0x48,
};
enum Status : uint8_t {
  ST_OK = 0x00, ST_BAD_CRC = 0x01, ST_BAD_LEN = 0x02, ST_BAD_TYPE = 0x03, ST_BAD_STATE = 0x04,
  ST_ADDR_RANGE = 0x05, ST_ALIGN = 0x06, ST_NOT_ERASED = 0x07, ST_FLASH_ERR = 0x08, ST_VERIFY_FAIL = 0x09,
  ST_IMAGE_TOO_BIG = 0x0A, ST_NOT_VALID = 0x0B, ST_BUSY = 0x0C, ST_NACK_STATE = 0x0D,
  ST_VERSION_MISMATCH = 0x0E, ST_UNKNOWN = 0xFF,
};
// set_mode request payload byte 0
enum Mode : uint8_t { MODE_OFF = 0, MODE_GRID = 1, MODE_INVERTER = 2, MODE_AUTO = 3 };
constexpr uint32_t kBootloaderMagic = 0xB007B007u;

struct Frame {
  uint8_t type = 0;
  uint8_t seq = 0;
  uint8_t len = 0;
  uint8_t version = kVersion;
  uint8_t payload[kMaxPayload] = {};
};

// Encode into out (needs len + kOverhead bytes). Returns bytes written or 0.
size_t encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len, uint8_t *out, size_t outCap);

class Decoder {
 public:
  // Feed one byte with a monotonic ms timestamp. Returns true when f holds a
  // complete, CRC-valid frame of the supported version.
  bool feed(uint8_t byte, uint32_t nowMs, Frame &f);
  void reset() { state_ = 0; }
  uint32_t crcErrors() const { return crcErrors_; }
  uint32_t versionErrors() const { return versionErrors_; }
  uint32_t timeouts() const { return timeouts_; }

 private:
  uint8_t state_ = 0;  // 0 hunt, 1 len, 2 ver, 3 type, 4 seq, 5 payload, 6 crc lo, 7 crc hi
  Frame work_;
  size_t idx_ = 0;
  uint16_t crcRx_ = 0;
  uint32_t lastMs_ = 0;
  uint32_t crcErrors_ = 0, versionErrors_ = 0, timeouts_ = 0;
};

// Status payload v1, agreed with the STM32 side (little endian, 30 bytes):
//  0 u8 sys            1 u8 switch          2 u8 charge          3 u8 restart_count
//  4 u32 fault_bits    8 u16 bat_mV        10 u8 bat_pct (0xFF = unknown)
// 11 u8 transfer_flags (bit0 transfer active, bit1 grid-phase synced, bit2 VOUT_SCALE_CALIBRATED)
// 12 u16 grid_dV      14 i16 temp_dC      16 u16 vout_dV       18 u16 iout_cA
// 20 u16 chg_cA       22 u16 mod_x10000   24 u32 uptime_s
// 28 u16 valid_flags  (bit0 bat, bit1 grid, bit2 temp, bit3 output)
// Longer payloads are accepted (future fields appended); shorter are rejected.
constexpr size_t kStatusLen = 30;
enum ValidFlags : uint16_t { VALID_BAT = 0x01, VALID_GRID = 0x02, VALID_TEMP = 0x04, VALID_OUTPUT = 0x08 };
bool decodeStatus(const uint8_t *p, size_t len, Stm32Status &out);
size_t encodeStatus(const Stm32Status &s, uint32_t uptime_s, uint8_t *out, size_t cap);  // used by tests / simulator

inline void putU16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
inline void putU32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }
inline uint16_t getU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t getU32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
}  // namespace frame
}  // namespace pc
