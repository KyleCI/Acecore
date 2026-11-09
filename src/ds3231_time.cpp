#include "ds3231_time.h"

#include "lagos_time.h"

namespace powercell {
namespace {
constexpr uint8_t kOsf = 0x80;
constexpr uint8_t kEosc = 0x80;
constexpr uint8_t kHour12 = 0x40;
constexpr uint8_t kCentury = 0x80;

bool bcd_ok(uint8_t value, int max_hi) {
  const int hi = value >> 4;
  const int lo = value & 0x0F;
  return hi >= 0 && hi <= max_hi && lo <= 9;
}

int bcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

uint8_t to_bcd(int value) {
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

RtcStatus decode_time(const uint8_t raw[7], int64_t* unix_out, bool* hour_24) {
  // DS3231 seconds bit 7 is reserved 0. It is not the DS1307 clock-halt bit.
  if (raw[0] & 0x80) return RtcStatus::BadRegister;
  *hour_24 = (raw[2] & kHour12) == 0;
  if (!bcd_ok(raw[0], 5) || !bcd_ok(raw[1], 5) || !bcd_ok(raw[6], 9)) {
    return RtcStatus::BadBcd;
  }
  int hour = 0;
  if (*hour_24) {
    if (!bcd_ok(static_cast<uint8_t>(raw[2] & 0x3F), 2)) return RtcStatus::BadBcd;
    hour = bcd(static_cast<uint8_t>(raw[2] & 0x3F));
  } else {
    if (!bcd_ok(static_cast<uint8_t>(raw[2] & 0x1F), 1)) return RtcStatus::BadBcd;
    const int h12 = bcd(static_cast<uint8_t>(raw[2] & 0x1F));
    if (h12 < 1 || h12 > 12) return RtcStatus::BadCalendar;
    const bool pm = (raw[2] & 0x20) != 0;
    if (h12 == 12) hour = pm ? 12 : 0;
    else hour = pm ? h12 + 12 : h12;
  }
  if (!bcd_ok(static_cast<uint8_t>(raw[5] & 0x1F), 1) || !bcd_ok(raw[4], 3)) {
    return RtcStatus::BadBcd;
  }
  if (raw[3] < 1 || raw[3] > 7) return RtcStatus::BadCalendar;
  const int second = bcd(raw[0]);
  const int minute = bcd(raw[1]);
  const int date = bcd(raw[4]);
  const int month = bcd(static_cast<uint8_t>(raw[5] & 0x1F));
  const int year = ((raw[5] & kCentury) ? 2100 : 2000) + bcd(raw[6]);
  if (second > 59 || minute > 59 || hour > 23 || !civil_valid(year, month, date)) {
    return RtcStatus::BadCalendar;
  }
  *unix_out = unix_from_utc(year, month, date, hour, minute, second);
  return RtcStatus::Unanchored;
}
}  // namespace

RtcReading ds3231_decode(const uint8_t frame[kDs3231FrameBytes]) {
  RtcReading out;
  out.oscillator_stopped = (frame[0x0F] & kOsf) != 0;
  out.battery_oscillator_enabled = (frame[0x0E] & kEosc) == 0;
  if (out.oscillator_stopped) {
    out.status = RtcStatus::OscillatorStopped;
    return out;
  }
  out.status = decode_time(frame, &out.unix_utc, &out.hour_24);
  return out;
}

RtcReading ds3231_decode_pair(const uint8_t first[kDs3231FrameBytes],
                              const uint8_t second[kDs3231FrameBytes]) {
  const RtcReading a = ds3231_decode(first);
  const RtcReading b = ds3231_decode(second);
  if (a.status == RtcStatus::OscillatorStopped || b.status == RtcStatus::OscillatorStopped) {
    RtcReading out = b;
    out.status = RtcStatus::OscillatorStopped;
    out.oscillator_stopped = true;
    out.unix_utc = 0;
    return out;
  }
  if (a.status != RtcStatus::Unanchored) return a;
  if (b.status != RtcStatus::Unanchored) return b;
  const int64_t delta = b.unix_utc >= a.unix_utc ? b.unix_utc - a.unix_utc : a.unix_utc - b.unix_utc;
  if (delta > 1) {
    RtcReading out = b;
    out.status = RtcStatus::Incoherent;
    out.unix_utc = 0;
    return out;
  }
  return b;
}

bool ds3231_encode(int64_t unix_utc, uint8_t time_out[7], uint8_t* control, uint8_t* status) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!utc_civil(unix_utc, &year, &month, &day, &hour, &minute, &second)) return false;
  if (year < 2000 || year > 2099 || !civil_valid(year, month, day)) return false;
  const int64_t days = unix_utc / 86400;
  const int sunday0 = static_cast<int>((days + 4) % 7);
  time_out[0] = to_bcd(second);
  time_out[1] = to_bcd(minute);
  time_out[2] = to_bcd(hour);
  time_out[3] = static_cast<uint8_t>(sunday0 + 1);
  time_out[4] = to_bcd(day);
  time_out[5] = to_bcd(month);
  time_out[6] = to_bcd(year - 2000);
  if (control) *control = kDs3231ControlRun;
  if (status) *status = kDs3231StatusClear;
  return (time_out[0] & 0x80) == 0 && (time_out[2] & 0x40) == 0 && (time_out[5] & 0x80) == 0;
}

bool ds3231_readback_matches(const RtcReading& read_back, int64_t written_unix) {
  if (read_back.status != RtcStatus::Unanchored || read_back.oscillator_stopped) return false;
  if (!read_back.battery_oscillator_enabled || !read_back.hour_24) return false;
  const int64_t delta = read_back.unix_utc >= written_unix ? read_back.unix_utc - written_unix
                                                           : written_unix - read_back.unix_utc;
  return delta <= 2;
}

ClockQuality qualify_boot_clock(bool nvs_stamp_present, bool rtc_coherent, bool ntp_valid) {
  (void)nvs_stamp_present;
  if (ntp_valid) return ClockQuality::Anchored;
  if (rtc_coherent) return ClockQuality::RtcUnanchored;
  return ClockQuality::Invalid;
}

}  // namespace powercell
