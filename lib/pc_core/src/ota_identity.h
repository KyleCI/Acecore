#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace pc {
// Identity embedded in every image and searched for during install:
//   POWERCELL_FW|<kind>|<model>|<version>|END
// An image accepts an offer only when the version differs from the one it is
// running, the offer names this image's exact model, and the file contains
// that model's identity. Model esp32dev is not one of these images.

inline bool otaVersionOk(const char* running, const char* offered) {
  if (!offered || !offered[0] || !running) return false;
  const size_t n = strlen(offered);
  if (n == 0 || n > 32) return false;
  return strcmp(offered, running) != 0;
}

inline bool otaFormatIdentity(char* out, size_t cap, const char* kind, const char* model, const char* version) {
  if (!out || !kind || !model || !version) return false;
  const int n = snprintf(out, cap, "POWERCELL_FW|%s|%s|%s|END", kind, model, version);
  return n > 0 && static_cast<size_t>(n) < cap;
}

inline bool otaImageHasIdentity(const void* image, size_t n, const char* identity) {
  if (!image || !identity || !identity[0]) return false;
  const size_t idn = strlen(identity);
  if (n < idn) return false;
  const char* p = static_cast<const char*>(image);
  for (size_t i = 0; i + idn <= n; ++i) {
    if (memcmp(p + i, identity, idn) == 0) return true;
  }
  return false;
}

// offered_model is required. A missing or different model is refused.
inline bool otaModelAccepts(const void* image, size_t n, const char* own_model, const char* running_version,
                            const char* offered_version, const char* offered_model) {
  if (!own_model || !own_model[0]) return false;
  if (!offered_model || strcmp(offered_model, own_model) != 0) return false;
  if (!otaVersionOk(running_version, offered_version)) return false;
  char id[128];
  if (!otaFormatIdentity(id, sizeof id, "collector", own_model, offered_version)) return false;
  return otaImageHasIdentity(image, n, id);
}
}  // namespace pc
