#pragma once
// Local hotspot OTA (POST /ota), ported from collector 2.5.12 (esp32_fw 681ebf2). Pure, host-tested.
// Adapted to PR #5: the identity prefix carries this image's exact model
// (POWERCELL_FW|collector|<model>|), so an image built for another model has no matching
// tag and is refused. Same version is always refused (the cloud rule, otaVersionOk).
// An older version is refused unless ?force=1.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "ota_identity.h"

namespace pc {
namespace lanota {

// Compares dotted numeric versions ("2.5.13" vs "2.5.9"): <0, 0, >0. Non-numeric parts compare as 0.
inline int compareVersion(const char *a, const char *b) {
  while (*a || *b) {
    long x = 0, y = 0;
    while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
    while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
    if (x != y) return x < y ? -1 : 1;
    while (*a && *a != '.') ++a;
    while (*b && *b != '.') ++b;
    if (*a == '.') ++a;
    if (*b == '.') ++b;
  }
  return 0;
}

// Streams an image and extracts the version from "<prefix><version>|END",
// e.g. prefix "POWERCELL_FW|collector|pc-dsp011-5kw|". Works across chunk boundaries.
class TagScanner {
 public:
  explicit TagScanner(const char *prefix) : prefix_(prefix) {}
  void feed(const uint8_t *d, size_t n) {
    if (found_) return;
    tail_.append(reinterpret_cast<const char *>(d), n);
    for (;;) {
      size_t p = tail_.find(prefix_);
      if (p == std::string::npos) break;
      size_t v = p + prefix_.size();
      size_t e = tail_.find("|END", v);
      if (e != std::string::npos && e - v <= kMaxVer) {
        std::string ver = tail_.substr(v, e - v);
        if (validVersion(ver)) { version_ = ver; found_ = true; tail_.clear(); return; }
      } else if (e == std::string::npos && tail_.size() - v <= kMaxVer + 3) {
        tail_.erase(0, p);  // wait for the rest of this tag
        return;
      }
      tail_.erase(0, p + 1);  // not a real tag; look for the next one
    }
    size_t keep = prefix_.size() + kMaxVer + 4;
    if (tail_.size() > keep) tail_.erase(0, tail_.size() - keep);
  }
  bool found() const { return found_; }
  const std::string &version() const { return version_; }

 private:
  static constexpr size_t kMaxVer = 16;
  static bool validVersion(const std::string &v) {
    if (v.empty() || v.size() > kMaxVer) return false;
    for (char c : v) if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return v.front() != '.' && v.back() != '.';
  }
  std::string prefix_, tail_, version_;
  bool found_ = false;
};

enum class Verdict { Ok, NoTag, Downgrade, SameVersion };
inline Verdict judge(const char *running, const TagScanner &s, bool force) {
  if (!s.found()) return Verdict::NoTag;
  if (!otaVersionOk(running, s.version().c_str())) return Verdict::SameVersion;  // force does not override
  if (!force && compareVersion(s.version().c_str(), running) < 0) return Verdict::Downgrade;
  return Verdict::Ok;
}
inline const char *verdictName(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::NoTag: return "identity_or_model_mismatch";
    case Verdict::Downgrade: return "downgrade_refused";
    case Verdict::SameVersion: return "same_version";
  }
  return "?";
}
// 64 hex chars, case-insensitive equality of two digests.
inline bool hexEq(const char *a, const char *b) {
  if (!a || !b || std::strlen(a) != 64 || std::strlen(b) != 64) return false;
  for (int i = 0; i < 64; ++i) {
    char x = a[i] | 0x20, y = b[i] | 0x20;
    bool hx = (x >= '0' && x <= '9') || (x >= 'a' && x <= 'f');
    if (!hx || x != y) return false;
  }
  return true;
}
// hotspot_only has no cloud, so the WebSocket health rule can never confirm an image there.
// A healthy local run (heap above kHeapFloor) for kHotspotHealthyMs confirms it instead.
constexpr uint32_t kHotspotHealthyMs = 180000;
constexpr uint32_t kHeapFloor = 30000;
inline uint32_t confirmAfterMs(bool hotspotOnly, uint32_t cloudMs) { return hotspotOnly ? kHotspotHealthyMs : cloudMs; }
inline bool healthyNow(bool hotspotOnly, bool cloudHealthy, uint32_t freeHeap) {
  return hotspotOnly ? freeHeap > kHeapFloor : cloudHealthy;
}
}  // namespace lanota
}  // namespace pc
