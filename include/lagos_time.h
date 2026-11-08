#pragma once

#include <stdint.h>

namespace powercell {

constexpr int32_t kLagosOffsetSeconds = 3600;

bool civil_valid(int year, int month, int day);
bool leap_year(int year);
int64_t unix_from_utc(int year, int month, int day, int hour, int minute, int second);
bool utc_civil(int64_t unix_utc, int* year, int* month, int* day, int* hour, int* minute, int* second);
int64_t unix_from_lagos(int year, int month, int day, int hour, int minute, int second);
bool lagos_civil(int64_t unix_utc, int* year, int* month, int* day);
int lagos_day_id(int64_t unix_utc);
bool lagos_interval_crosses_midnight(int64_t start_utc, int64_t end_utc, uint32_t margin_s);

}  // namespace powercell
