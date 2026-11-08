#pragma once

#include <stdint.h>

namespace powercell {

enum class ClockQuality : uint8_t { Invalid = 0, RtcUnanchored = 1, Anchored = 2 };

enum class RtcStatus : uint8_t {
  Unanchored = 0,
  Halted,
  BadBcd,
  BadCalendar,
  Not24Hour,
  Incoherent,
};

struct RtcReading {
  RtcStatus status = RtcStatus::Incoherent;
  int64_t unix_utc = 0;
  bool ch_clear = false;
};

RtcReading ds1307_decode_pair(const uint8_t first[7], const uint8_t second[7]);
ClockQuality qualify_boot_clock(bool nvs_stamp_present, bool rtc_coherent, bool ntp_valid);

}  // namespace powercell
