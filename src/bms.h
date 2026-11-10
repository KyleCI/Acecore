#pragma once
#include <Arduino.h>
#include "jk_bms.h"

// STM images: JK-PB BMS on Serial2 (IO17 RX / IO16 TX, 115200 8N1). The BMS
// streams 300-byte frames. DSP images: JBD polled at 9600 8N1, default
// IO16 RX / IO17 TX (DSP_BMS_UART_SWAP selects the other pair). A bad frame
// never publishes zeros: the last good reading ages out after JK_BMS_STALE_MS.
namespace bms {
struct Stats { uint32_t frames = 0, ok = 0, setupFrames = 0, checksumErrors = 0, badFrames = 0, timeouts = 0; };
void begin();
bool read(pc::jk::Data &out, uint32_t *ageMs = nullptr);  // latest reading if fresh
bool sample(pc::jk::Data &out, uint32_t *ageMs, uint32_t maxAgeMs);  // same frame, caller picks the age limit
size_t rawDump(Print &out, size_t n, uint32_t timeoutMs);  // TEMPORARY: BMS RAW console
bool online();
Stats stats();
}  // namespace bms
