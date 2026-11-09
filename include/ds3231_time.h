#pragma once

#include <stddef.h>
#include <stdint.h>

namespace powercell {

// DS3231 timekeeping, Maxim register map. Not a DS1307: register 0x00 bit 7
// is not a clock-halt, there is no battery RAM, and validity is the status
// oscillator-stop flag (OSF, 0x0F bit 7) plus the calendar registers.
enum class ClockQuality : uint8_t { Invalid = 0, RtcUnanchored = 1, Anchored = 2 };

enum class RtcStatus : uint8_t {
  Unanchored = 0,
  OscillatorStopped,
  BadBcd,
  BadCalendar,
  BadRegister,
  Incoherent,
  ShortRead,
};

struct RtcReading {
  RtcStatus status = RtcStatus::Incoherent;
  int64_t unix_utc = 0;
  bool oscillator_stopped = true;
  // Control 0x0E bit 7 (EOSC) clear: the oscillator keeps running on VBAT.
  bool battery_oscillator_enabled = false;
  bool hour_24 = false;
};

constexpr size_t kDs3231FrameBytes = 16;
constexpr uint8_t kDs3231Address = 0x68;
constexpr uint8_t kDs3231ControlRun = 0x04;  // EOSC=0, INTCN=1, alarms off
constexpr uint8_t kDs3231StatusClear = 0x00;  // OSF clear, 32 kHz output off

// 16-byte read starting at register 0x00 (time, alarms, control, status).
RtcReading ds3231_decode(const uint8_t frame[kDs3231FrameBytes]);
RtcReading ds3231_decode_pair(const uint8_t first[kDs3231FrameBytes],
                              const uint8_t second[kDs3231FrameBytes]);
// 24-hour time, century bit clear (years 2000-2099), EOSC clear.
bool ds3231_encode(int64_t unix_utc, uint8_t time_out[7], uint8_t* control, uint8_t* status);
bool ds3231_readback_matches(const RtcReading& read_back, int64_t written_unix);
ClockQuality qualify_boot_clock(bool nvs_stamp_present, bool rtc_coherent, bool ntp_valid);

}  // namespace powercell
