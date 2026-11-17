#pragma once
#include <Arduino.h>
#include "jk_bms.h"

// STM images: JK-PB BMS on Serial2 (IO17 RX / IO16 TX, 115200 8N1). The BMS
// streams 300-byte frames. DSP images use the same Serial2 pins: ESP RX
// GPIO17, TX GPIO16 (Dubem's working firmware 2026-11-10, confirmed by Caleb
// 2026-11-11 18:30 WAT; DSP_BMS_UART_SWAP stays 0). Both DSP ratings listen
// for JK-PB frames at 115200 (DSP_BMS_JKPB=1). The JBD 9600 poller on
// RX16/TX17 (DSP_BMS_JKPB=0) is kept in the tree but no image uses it. A bad frame never publishes zeros: the
// last good reading ages out after JK_BMS_STALE_MS.
namespace bms {
struct Stats { uint32_t frames = 0, ok = 0, setupFrames = 0, checksumErrors = 0, badFrames = 0, timeouts = 0; };
void begin();
bool read(pc::jk::Data &out, uint32_t *ageMs = nullptr);  // latest reading if fresh
bool sample(pc::jk::Data &out, uint32_t *ageMs, uint32_t maxAgeMs);  // same frame, caller picks the age limit
size_t rawDump(Print &out, size_t n, uint32_t timeoutMs);  // TEMPORARY: BMS RAW console
bool online();
Stats stats();
}  // namespace bms
