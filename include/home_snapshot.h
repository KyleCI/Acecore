#pragma once

#include <stddef.h>
#include <stdint.h>

namespace powercell {

// Installed Powercore c874f427 reads GET /data, not root metric names.
// Active watts stay in watts. Apparent VA is V*I and only under output_power.
// Null is missing. This object does not scale a reading to hide displayKw.

inline bool bms_sample_fresh(bool frame_ok, uint32_t received_at_ms, uint32_t now_ms) {
  return frame_ok && static_cast<uint32_t>(now_ms - received_at_ms) <= 15000u;
}

inline bool ac_sample_fresh(bool sample_ok, uint32_t sampled_at_ms, uint32_t now_ms) {
  return sample_ok && static_cast<uint32_t>(now_ms - sampled_at_ms) <= 2000u;
}

struct HomeMeters {
  const char* device_id = nullptr;
  bool output_on = false;
  bool ac_valid = false;
  double volts = 0;
  double amps = 0;
  double active_w = 0;
  bool meter_present = false;
  bool has_lifetime_wh = false;
  uint32_t lifetime_wh = 0;
  bool bms_valid = false;
  double soc = 0;
  double bat_v = 0;
  double bat_a = 0;
  double bat_w = 0;
  double remain_ah = 0;
  double full_ah = 0;
  int cycles = 0;
  double soh = 0;
  double bat_temp = 0;
  const char* bat_status = nullptr;
  bool pv_valid = false;
  double pv_v = 0;
  double pv_a = 0;
  double pv_w = 0;
  bool has_usable = false;
  double usable_kw = 0;
  const char* powercell_model = nullptr;
};

// Writes the document GET /data and the SSE state event return.
// note_a and note_b are raw JSON objects, or null. Returns the length, or -1.
int home_snapshot_write(const HomeMeters& meters, const char* note_a, const char* note_b,
                        char* out, size_t cap);

}  // namespace powercell
