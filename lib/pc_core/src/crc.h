#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pc {
// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0.
// Check value for "123456789" is 0x29B1.
uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);
// CRC-16/MODBUS: reflected poly 0xA001, init 0xFFFF. Check value 0x4B37.
// Sent low byte first on the wire.
uint16_t crc16_modbus(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);
// CRC-32/IEEE (zlib): reflected poly 0xEDB88320, init/xorout 0xFFFFFFFF.
// Check value for "123456789" is 0xCBF43926. Pass the previous result to continue.
uint32_t crc32_ieee(const uint8_t *data, size_t len, uint32_t prev = 0);
}  // namespace pc
