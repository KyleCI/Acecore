#pragma once

// One Powercore app reads every Powercell. The collector converts each source
// into watts before it fills an existing power key. A source that reports
// kilowatts is multiplied by 1000 here. Apparent VA stays on the apparent keys
// and is never copied into an active-watt key. A 15 W reading stays 15.

namespace powercell {

// Fixed for this firmware build. GET /v2/setup/info publishes both.
// App contract: letambari/official-powercell-app PR 8,
// powercell-app/docs/setup_info_units.md.
// units.power is W for unit-less power keys: last_data.power, inverter.output_power
// when it is not apparent_va, battery.power, and solar.power.
// units.energy is Wh for unit-less energy such as inverter_energy.
// Names that already carry a unit stay as named: usable_kw, *_w, lifetime_wh,
// energy_today_kwh, rated_kva. apparent_va values stay volt-amps.
// STM32 has no watt measurement, so those watt keys are JSON null.
constexpr int kDataFormatVersion = 1;
constexpr char kUnitlessPower[] = "W";
constexpr char kUnitlessEnergy[] = "Wh";

// The app treats a negative power, or a power above 1.5 times usable, as
// unavailable. Placeholder sentinels must be JSON null, not a number.
inline bool power_publishable(double watts) {
  if (watts != watts) return false;
  if (watts > 1.0e12 || watts < -1.0e12) return false;
  const double sentinels[] = {-1.0, 99999.0};
  for (int i = 0; i < 2; ++i) {
    double delta = watts - sentinels[i];
    if (delta < 0) delta = -delta;
    if (delta < 0.001) return false;
  }
  return true;
}

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
    const double watts = in.active_scale == PowerScale::Kilowatts ? in.active * 1000.0 : in.active;
    if (power_publishable(watts)) {
      out.have_active = true;
      out.active_w = watts;
    }
  }
  if (in.voltage_present && in.current_present) {
    const double va = in.volts * in.amps;
    if (power_publishable(va)) {
      out.have_apparent = true;
      out.apparent_va = va;
    }
  }
  return out;
}

}  // namespace powercell
