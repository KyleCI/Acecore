#include "ds1307_time.h"

#include "lagos_time.h"

namespace powercell {
namespace {
bool bcd_ok(uint8_t value, int max_hi) {
  const int hi = value >> 4;
  const int lo = value & 0x0F;
  return hi >= 0 && hi <= max_hi && lo >= 0 && lo <= 9;
}

int bcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

RtcStatus decode_one(const uint8_t raw[7], int64_t* unix_out, bool* ch_clear) {
  *ch_clear = (raw[0] & 0x80) == 0;
  if (raw[0] & 0x80) return RtcStatus::Halted;
  if (raw[2] & 0x40) return RtcStatus::Not24Hour;
  if (!bcd_ok(static_cast<uint8_t>(raw[0] & 0x7F), 5) || !bcd_ok(raw[1], 5) ||
      !bcd_ok(static_cast<uint8_t>(raw[2] & 0x3F), 2) || !bcd_ok(raw[4], 3) ||
      !bcd_ok(raw[5], 1) || !bcd_ok(raw[6], 9)) {
    return RtcStatus::BadBcd;
  }
  const int second = bcd(static_cast<uint8_t>(raw[0] & 0x7F));
  const int minute = bcd(raw[1]);
  const int hour = bcd(static_cast<uint8_t>(raw[2] & 0x3F));
  const int date = bcd(raw[4]);
  const int month = bcd(raw[5]);
  const int year = 2000 + bcd(raw[6]);
  if (second > 59 || minute > 59 || hour > 23 || !civil_valid(year, month, date)) {
    return RtcStatus::BadCalendar;
  }
  *unix_out = unix_from_utc(year, month, date, hour, minute, second);
  return RtcStatus::Unanchored;
}
}  // namespace

RtcReading ds1307_decode_pair(const uint8_t first[7], const uint8_t second[7]) {
  RtcReading out;
  int64_t a = 0, b = 0;
  bool ch_a = false, ch_b = false;
  const RtcStatus sa = decode_one(first, &a, &ch_a);
  const RtcStatus sb = decode_one(second, &b, &ch_b);
  out.ch_clear = ch_a && ch_b;
  if (sa == RtcStatus::Halted || sb == RtcStatus::Halted) {
    out.status = RtcStatus::Halted;
    return out;
  }
  if (sa != RtcStatus::Unanchored) {
    out.status = sa;
    return out;
  }
  if (sb != RtcStatus::Unanchored) {
    out.status = sb;
    return out;
  }
  const int64_t delta = b >= a ? b - a : a - b;
  if (delta > 1) {
    out.status = RtcStatus::Incoherent;
    return out;
  }
  out.status = RtcStatus::Unanchored;
  out.unix_utc = b;
  return out;
}

ClockQuality qualify_boot_clock(bool nvs_stamp_present, bool rtc_coherent, bool ntp_valid) {
  (void)nvs_stamp_present;
  if (ntp_valid) return ClockQuality::Anchored;
  if (rtc_coherent) return ClockQuality::RtcUnanchored;
  return ClockQuality::Invalid;
}

}  // namespace powercell
