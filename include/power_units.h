#pragma once

// One Powercore app reads every Powercell. The collector converts each source
// into watts before it fills an existing power key. A source that reports
// kilowatts is multiplied by 1000 here. Apparent VA stays on the apparent keys
// and is never copied into an active-watt key. A 15 W reading stays 15.

namespace powercell {

enum class PowerScale : unsigned char { Watts = 0, Kilowatts = 1 };

struct OutputSample {
  bool voltage_present = false;
  double volts = 0;
  bool current_present = false;
  double amps = 0;
  bool active_present = false;
  double active = 0;
  PowerScale active_scale = PowerScale::Watts;
};

struct NormalizedOutput {
  bool have_voltage = false;
  bool have_current = false;
  bool have_active = false;
  bool have_apparent = false;
  double volts = 0;
  double amps = 0;
  double active_w = 0;
  double apparent_va = 0;
};

inline NormalizedOutput normalize_output(const OutputSample& in) {
  NormalizedOutput out;
  out.have_voltage = in.voltage_present;
  out.have_current = in.current_present;
  out.volts = in.voltage_present ? in.volts : 0;
  out.amps = in.current_present ? in.amps : 0;
  if (in.active_present) {
    out.have_active = true;
    out.active_w = in.active_scale == PowerScale::Kilowatts ? in.active * 1000.0 : in.active;
  }
  if (in.voltage_present && in.current_present) {
    out.have_apparent = true;
    out.apparent_va = in.volts * in.amps;
  }
  return out;
}

}  // namespace powercell
