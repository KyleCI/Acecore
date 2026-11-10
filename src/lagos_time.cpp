#include "lagos_time.h"

namespace powercell {
namespace {
int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int* year, unsigned* month, unsigned* day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  *year = y + (m <= 2);
  *month = m;
  *day = d;
}
}  // namespace

bool leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

bool civil_valid(int year, int month, int day) {
  if (month < 1 || month > 12 || day < 1 || year < 2000 || year > 2099) return false;
  static const int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int dim = kDays[month];
  if (month == 2 && leap_year(year)) dim = 29;
  return day <= dim;
}

int64_t unix_from_utc(int year, int month, int day, int hour, int minute, int second) {
  if (!civil_valid(year, month, day) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return 0;
  }
  const int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

bool utc_civil(int64_t unix_utc, int* year, int* month, int* day, int* hour, int* minute, int* second) {
  if (unix_utc < 0 || !year || !month || !day) return false;
  int64_t days = unix_utc / 86400;
  int rem = static_cast<int>(unix_utc % 86400);
  unsigned m = 0, d = 0;
  civil_from_days(days, year, &m, &d);
  *month = static_cast<int>(m);
  *day = static_cast<int>(d);
  if (hour) *hour = rem / 3600;
  if (minute) *minute = (rem % 3600) / 60;
  if (second) *second = rem % 60;
  return true;
}

int64_t unix_from_lagos(int year, int month, int day, int hour, int minute, int second) {
  const int64_t local = unix_from_utc(year, month, day, hour, minute, second);
  if (local == 0 && !(year == 2000 && month == 1 && day == 1 && hour == 0 && minute == 0 && second == 0)) {
    return 0;
  }
  return local - kLagosOffsetSeconds;
}

bool lagos_civil(int64_t unix_utc, int* year, int* month, int* day) {
  return utc_civil(unix_utc + kLagosOffsetSeconds, year, month, day, nullptr, nullptr, nullptr);
}

int lagos_day_id(int64_t unix_utc) {
  int y = 0, m = 0, d = 0;
  if (!lagos_civil(unix_utc, &y, &m, &d)) return 0;
  return y * 10000 + m * 100 + d;
}

bool lagos_interval_crosses_midnight(int64_t start_utc, int64_t end_utc, uint32_t margin_s) {
  if (end_utc < start_utc) return true;
  const int64_t margin = static_cast<int64_t>(margin_s);
  const int64_t a = start_utc - margin;
  const int64_t b = end_utc + margin;
  if (a < 0) return true;
  return lagos_day_id(a) != lagos_day_id(b);
}

}  // namespace powercell
