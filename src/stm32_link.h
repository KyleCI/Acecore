#pragma once
#include <Arduino.h>
#include "config.h"
#include "stm32_frame.h"
#include "stm32_types.h"

// ESP32 <-> STM32 link on Serial1 (RX IO33 <- PB6, TX IO2 -> PB7, no RTS).
// The byte parser is pluggable:
//   Protocol::LegacyText - today's STM32 printf telemetry at 9600, receive-only.
//   Protocol::Framed     - agreed binary frames at 115200 with commands.
// The protocol comes from STM32_LINK_PROTOCOL, overridable at runtime with
// the console command "SET stm32_proto 0|1" (stored in NVS, applied on reboot).
namespace stm32link {
enum class Protocol : uint8_t { LegacyText = 0, Framed = 1 };
enum class Result : uint8_t { Ok, Nack, Timeout, NotSupported, Busy, BadState, BadResponse };
const char *resultName(Result r);

struct Stats {
  uint32_t bytes = 0, records = 0, crcErrors = 0, versionErrors = 0, timeouts = 0, lineOverflows = 0, cmdTimeouts = 0;
};

void begin();
Protocol protocol();

// Latest status. Returns false if none has been received yet.
// ageMs is the time since it arrived.
bool status(pc::Stm32Status &out, uint32_t *ageMs = nullptr);
bool online();  // status received within STM32_STALE_MS
Stats stats();

// Commands (framed protocol only). Blocking up to STM32_CMD_TIMEOUT_MS x retries;
// call from the main loop or a worker task, never from a callback/ISR.
Result setMode(pc::frame::Mode mode, uint8_t *nackStatus = nullptr);
Result faultReset();
Result getVersion(uint32_t &fwVersion, uint8_t &protoVersion);
// Refused locally unless the last status says SYS_IDLE; the STM32 re-checks.
Result enterBootloader();
// Generic request/response (used by the STM32 updater).
Result transact(uint8_t type, const uint8_t *payload, size_t len, pc::frame::Frame &resp,
                uint32_t timeoutMs = STM32_CMD_TIMEOUT_MS, uint8_t retries = STM32_CMD_RETRIES);
// Pulse NRST through IO26 (FEATURE_STM32_NRST only). Returns false when not fitted.
bool pulseReset();

// STM32 v2.1 text commands on the legacy 9600 link (line ends in \n, max 47 chars,
// case sensitive). Only the read-only queries are exposed; CAL VOUT/SAVE/CLEAR are
// never sent from here. Reply goes to out (e.g. "VER PC-INV-2.1.0 <date> <time>").
Result textQuery(const char *cmd, const char *replyPrefix, char *out, size_t n, uint32_t timeoutMs = 1500);
struct CalInfo { bool valid = false; float scale = 0, def = 0; bool stored = false, active = false, pending = false; };
bool parseCal(const char *reply, CalInfo &out);  // "OK CAL scale=.. default=.. stored=0|1 active=0|1 pending=0|1"
// Cached results of the last successful VER?/CAL? (empty/invalid when never answered).
String lastVersion();
}  // namespace stm32link
