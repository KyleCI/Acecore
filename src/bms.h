#pragma once
#include <Arduino.h>
#include "jk_bms.h"

// JK-PB BMS on Serial2 (IO16 RX / IO17 TX, JK_BMS_BAUD 8N1) through an
// auto-direction TTL-RS485 module. The BMS streams 300-byte frames; a task on
// core 1 assembles them and keeps the latest valid DYNAMIC frame. Checksum or
// framing errors never publish zeros: the last good reading ages out and the
// BMS is reported offline (all BMS fields null) after JK_BMS_STALE_MS.
namespace bms {
struct Stats { uint32_t frames = 0, ok = 0, setupFrames = 0, checksumErrors = 0, badFrames = 0, timeouts = 0; };
void begin();
bool read(pc::jk::Data &out, uint32_t *ageMs = nullptr);  // latest reading if fresh
bool online();
Stats stats();
}  // namespace bms
