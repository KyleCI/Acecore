#pragma once
#include <stdint.h>

// Receiver-side reading of the legacy STM32 text stream.
// Limits below are the archived PC-CALEB-XFER4 notes (not a calibration of an
// installed board, and not permission to integrate live energy):
//   VOUT is SPWM_GetOutputVoltageRMS over a 20 ms PA0 PWM ISR window.
//   VoutValid is not transmitted. VOUT_SCALE_CALIBRATED is 0.
//   IOUT is a main-loop 20 ms above-zero PA2 half-wave value, zero 1976,
//   scale 20.86, and the sender clamps results below 0.18 A.
//   VOUT/IOUT are emitted only while SYS is INVERTER. SYS GRID emits charging
//   fields, not load V/I.
// The 20 ms figures are sender windows. This receiver does not reapply
// 1976/20.86. The text token is treated as the sender's already scaled
// number. The printf format itself was not in the notes supplied here.

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
  bool provisional_inverter_only;
  bool live_eligible;
  bool mutex_failed;
  bool stale;
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
  bool publish(uint32_t mono_ms, TextCandidate *out);
  bool take_number(const char *value, int32_t *milli);

  TextValidityConfig cfg_;
  bool open_;
  bool banner_;
  bool mutex_failed_;
  TextMode mode_;
  TextSwitch sw_;
  bool v_seen_, i_seen_, v_ok_, i_ok_;
  int32_t v_mv_, i_ma_;
  uint32_t opened_ms_, v_ms_, i_ms_;
  bool have_v_ms_, have_i_ms_;
};

}  // namespace loadac
