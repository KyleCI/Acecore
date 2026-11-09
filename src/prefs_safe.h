#pragma once
// Quiet Preferences helpers: the Arduino Preferences class logs an [E] line for a
// read-only open of a namespace that was never written and for getString /
// getBytesLength on a missing key. On a normal boot those are expected, so check first.
#include <Preferences.h>
#include <nvs.h>

inline bool nvsNsExists(const char *ns) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;  // raw call: no log
  nvs_close(h);
  return true;
}
inline bool prefsOpenRO(Preferences &p, const char *ns) { return nvsNsExists(ns) && p.begin(ns, true); }
inline String prefsStr(Preferences &p, const char *key, const char *def = "") { return p.isKey(key) ? p.getString(key, def) : String(def); }
inline size_t prefsLen(Preferences &p, const char *key) { return p.isKey(key) ? p.getBytesLength(key) : 0; }
