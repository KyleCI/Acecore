#pragma once

#include "crc32_ieee.h"

#include <stdint.h>
#include <string.h>

namespace powercell {

constexpr uint32_t kSlotBytes = 128;
constexpr uint32_t kSlotPayload = 64;
constexpr uint16_t kSlotMagic = 0xA55A;

enum SlotKind : uint8_t {
  KindEnergyInterval = 1,
  KindEnergyDiscontinuity = 2,
  KindEnergyCorrection = 3,
  KindDiagBoot = 10,
  KindDiagReset = 11,
  KindDiagConsole = 12,
  KindDiagStm32 = 13,
  KindDiagStm32Bad = 14,
  KindDiagBms = 15,
  KindDiagBmsBad = 16,
  KindDiagPv = 17,
  KindDiagPzem = 18,
  KindDiagPzemBad = 19,
  KindDiagState = 20,
  KindDiagLink = 21,
  KindDiagClock = 22,
  KindDiagStorage = 23,
  KindDiagFault = 24,
  KindDiagIncident = 25,
};

enum SlotFlag : uint8_t {
  FlagTruncatedText = 1u << 0,
  FlagMeasurementGap = 1u << 1,
};

struct SlotRecord {
  uint32_t seq = 0;
  uint32_t mono_ms = 0;
  int64_t source_unix = 0;
  int64_t recv_unix = 0;
  uint8_t clock_quality = 0;
  uint8_t kind = 0;
  uint8_t flags = 0;
  uint8_t len = 0;
  uint8_t payload[kSlotPayload] = {};
  bool acked = false;
};

inline void put_u16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void put_i32(uint8_t* p, int32_t v) { put_u32(p, static_cast<uint32_t>(v)); }
inline void put_i64(uint8_t* p, int64_t v) {
  const uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(u >> (8 * i));
}
inline uint16_t get_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
inline int32_t get_i32(const uint8_t* p) { return static_cast<int32_t>(get_u32(p)); }
inline int64_t get_i64(const uint8_t* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(p[i]) << (8 * i);
  return static_cast<int64_t>(u);
}

inline bool slot_is_blank(const uint8_t raw[kSlotBytes]) {
  for (uint32_t i = 0; i < kSlotBytes; ++i) {
    if (raw[i] != 0xFF) return false;
  }
  return true;
}

inline void slot_encode(const SlotRecord& in, uint8_t raw[kSlotBytes]) {
  memset(raw, 0xFF, kSlotBytes);
  put_u16(raw + 0, kSlotMagic);
  raw[2] = 1;
  raw[3] = in.kind;
  put_u32(raw + 4, in.seq);
  put_u32(raw + 8, in.mono_ms);
  put_i64(raw + 12, in.source_unix);
  put_i64(raw + 20, in.recv_unix);
  raw[28] = in.clock_quality;
  raw[29] = in.flags;
  raw[30] = in.len > kSlotPayload ? kSlotPayload : in.len;
  raw[31] = 0;
  memcpy(raw + 32, in.payload, raw[30]);
  if (raw[30] < kSlotPayload) memset(raw + 32 + raw[30], 0, kSlotPayload - raw[30]);
  put_u32(raw + 96, ::crc32_ieee(raw, 96));
  raw[100] = in.acked ? 0xFE : 0xFF;
}

inline bool slot_decode(const uint8_t raw[kSlotBytes], SlotRecord* out) {
  if (slot_is_blank(raw)) return false;
  if (get_u16(raw) != kSlotMagic || raw[2] != 1) return false;
  if (get_u32(raw + 96) != ::crc32_ieee(raw, 96)) return false;
  out->kind = raw[3];
  out->seq = get_u32(raw + 4);
  out->mono_ms = get_u32(raw + 8);
  out->source_unix = get_i64(raw + 12);
  out->recv_unix = get_i64(raw + 20);
  out->clock_quality = raw[28];
  out->flags = raw[29];
  out->len = raw[30] > kSlotPayload ? kSlotPayload : raw[30];
  memcpy(out->payload, raw + 32, kSlotPayload);
  out->acked = raw[100] != 0xFF;
  return out->seq != 0;
}

inline bool slot_is_fault_mirror(uint8_t kind) {
  return kind == KindDiagBoot || kind == KindDiagReset || kind == KindDiagBmsBad ||
         kind == KindDiagPzemBad || kind == KindDiagState || kind == KindDiagStorage ||
         kind == KindDiagFault || kind == KindDiagIncident || kind == KindDiagClock;
}

}  // namespace powercell
