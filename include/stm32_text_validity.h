#pragma once
#include <stdint.h>

// Receiver-side reading of the PC-CALEB-XFER4 text stream.
// The flashed image contains these format strings. The candidate source
// prints them on USART1 at 9600 8N1. A report is three 20 ms ticks, and the
// next report starts TELEM_PERIOD_TICKS (100) later, which is 2 s. Phase 2
// prints VOUT and IOUT only while SYS is INVERTER:
//   VOUT: %.1fV    SPWM RMS counts times VOUT_ADC_SCALE. VoutValid is not
//                  sent. VOUT_SCALE_CALIBRATED is 0, so this is not a
//                  calibrated RMS claim.
//   IOUT: %.2fA    20 ms above-zero PA2 half-wave, zero 1976, scale 20.86,
//                  and values below 0.18 A are printed as 0.00. That is the
//                  sender noise clamp, not a measured no-load.
// SYS GRID prints charger fields instead of VOUT/IOUT. Phase 3 may print
// FAULT and TXDROP, then the exact closer "--------------".
// Rebuilding that source was not proven (no manifest, DWARF, or build log),
// so a parsed record is not permission to integrate live energy.
// max_same_record_skew_ms bounds only the VOUT and IOUT timestamps. It is
// not the 2 s report period.

namespace loadac {

enum class TextMode : uint8_t { Unknown = 0, Idle, Inverter, Grid, Fault, Reserve };
enum class TextSwitch : uint8_t { Unknown = 0, Inverter, Grid };

struct TextValidityConfig {
  int32_t max_voltage_mv;
  int32_t max_current_ma;
  int32_t clamp_ambiguous_below_ma;
  uint32_t max_same_record_skew_ms;
};

struct LineageGate {
  bool installed_lineage_verified;
  bool rms_validity_transmitted;
};

struct TextCandidate {
  bool closed;
  bool complete;
  bool truncated;
  bool provisional_inverter_only;
  bool live_eligible;
  bool mutex_failed;
  bool stale;
  bool txdrop;
  bool fault_line;
  bool banner_xfer4_seen;
  bool v_in_record;
  bool i_in_record;
  bool v_numeric;
  bool i_numeric;
  TextMode mode;
  TextSwitch sw;
  int32_t v_mv;
  int32_t i_ma;
  uint32_t opened_ms;
  uint32_t closed_ms;
  const char *reason;
};

TextValidityConfig default_text_limits();
LineageGate closed_lineage_gate();
bool live_estimate_allowed(const LineageGate &gate, const TextCandidate &candidate);

class Stm32TextAssembler {
 public:
  explicit Stm32TextAssembler(TextValidityConfig cfg = default_text_limits());
  void reset_boot();
  void note_mutex_failure(uint32_t mono_ms, TextCandidate *out);
  bool feed_line(uint32_t mono_ms, const char *line, TextCandidate *published);
  bool banner_seen() const { return banner_; }

 private:
  void clear_fields();
  bool publish(uint32_t mono_ms, TextCandidate *out, bool by_terminator);
  bool take_number(const char *value, int32_t *milli, char unit);

  TextValidityConfig cfg_;
  bool open_;
  bool banner_;
  bool mutex_failed_;
  bool txdrop_;
  bool fault_line_;
  TextMode mode_;
  TextSwitch sw_;
  bool v_seen_, i_seen_, v_ok_, i_ok_;
  int32_t v_mv_, i_ma_;
  uint32_t opened_ms_, v_ms_, i_ms_;
  bool have_v_ms_, have_i_ms_;
};

}  // namespace loadac
