#pragma once
#include <stddef.h>
#include <stdint.h>
#include "stm32_types.h"

namespace pc {
// Parser for the STM32's current human-readable telemetry (9600 8N1):
//   --- Status ---
//   SYS : IDLE|GRID|INVERTER|FAULT
//   SW  : INVERTER|GRID
//   BAT : 12.80V  55%
//   GRID: 229.5V
//   TEMP: 31.2C
//   CHR : IDLE|WAIT|BULK|ABSORB|FLOAT|FAULT|--
//   VOUT: 230.1V   IOUT: 1.23A
//   >>> FAULT: 0x21 <<<
// Event lines outside the record are mapped onto the framed status flags so
// both links feed the event engine the same fault_bits:
//   "FAULT: Permanent fault ..."      -> bit17 (until SYS leaves FAULT)
//   "FAULT: Max restart attempts ..." -> bit18 (until SYS leaves FAULT)
//   "CHR: Battery confirmed full"     -> bit19 (until CHR is BULK/ABSORB/OFF or a "CHR: FAULT")
// "WARN: High current" is not mapped to bit16: it has no clear line, so the
// legacy link uses the ESP-derived overload rule instead.
// A record starts at "--- Status ---" or "SYS" and is published when the next
// record starts or after an idle gap (flushIdle). Lines are bounded: a line
// longer than kMaxLine is discarded rather than growing without limit.
class LegacyTextParser {
 public:
  static constexpr size_t kMaxLine = 96;
  // Feed one byte; returns true when a complete record was published to out.
  bool feed(uint8_t byte, Stm32Status &out);
  // Call when the line has been quiet for a while; publishes a partial record.
  bool flushIdle(Stm32Status &out);
  uint32_t overflows() const { return overflows_; }
  uint32_t lines() const { return lines_; }
  // STM32 v2.1 command replies ("VER ...", "OK ...", "ERR ...") are mixed into the
  // status stream; they are never parsed as record fields but kept here instead.
  // replySeq() increments for each one; reply() is the latest line (NUL-terminated).
  uint32_t replySeq() const { return replySeq_; }
  const char *reply() const { return reply_; }
  static bool isReplyLine(const char *s);
  void reset();

 private:
  bool handleLine(Stm32Status &out);
  bool publish(Stm32Status &out);
  char line_[kMaxLine + 1] = {};
  size_t len_ = 0;
  bool overflow_ = false;
  bool inRecord_ = false;
  Stm32Status work_;
  uint32_t seq_ = 0, overflows_ = 0, lines_ = 0;
  uint32_t lastFault_ = 0;
  uint32_t flags_ = 0;
  char reply_[kMaxLine + 1] = {};
  uint32_t replySeq_ = 0;  // status bits 16..19 synthesized from text event lines
};
}  // namespace pc
