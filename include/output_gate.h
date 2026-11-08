#pragma once

#include <string.h>

// 2.5.11 gateway.cpp: a payment lock refuses user on and user off.
// System off (battery_critical, ota, payment_lock) still runs. This collector
// does not set the flag. It only honors pc_ident/pay_lock when that key exists.
inline bool user_output_on_allowed(bool battery_allows, bool overload_latched) {
  return battery_allows && !overload_latched;
}

inline bool payment_allows_output(bool payment_locked, bool turning_on, const char* origin) {
  if (!payment_locked) return true;
  if (turning_on) return false;
  if (!origin) return false;
  return strcmp(origin, "battery_critical") == 0 || strcmp(origin, "ota") == 0 ||
         strcmp(origin, "payment_lock") == 0 || strcmp(origin, "ac_overload") == 0;
}
