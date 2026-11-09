#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace pc {
// Identity embedded in every image and searched for during install:
//   POWERCELL_FW|<kind>|<model>|<version>|END
// Deployed 2.5.11 (ota.cpp) accepts an image when the offered version differs
// from the running one and that exact string is present. It does not read an
// offer "model" field. The running model on those units is esp32dev.

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

// Unchanged 2.5.11 install rule. Model is esp32dev. An offer model is ignored.
inline bool otaStock2511Accepts(const void* image, size_t n, const char* offered_version) {
  if (!otaVersionOk("2.5.11", offered_version)) return false;
  char id[96];
  if (!otaFormatIdentity(id, sizeof id, "collector", "esp32dev", offered_version)) return false;
  return otaImageHasIdentity(image, n, id);
}

// Hardware image. offered_model == nullptr means the check response omitted model.
// A present model that is not this image's model is refused, even if the bytes match.
inline bool otaHwAccepts(const void* image, size_t n, const char* hw_model, const char* running_version,
                         const char* offered_version, const char* offered_model) {
  if (!hw_model || !hw_model[0]) return false;
  if (!otaVersionOk(running_version, offered_version)) return false;
  if (offered_model && strcmp(offered_model, hw_model) != 0) return false;
  char id[128];
  if (!otaFormatIdentity(id, sizeof id, "collector", hw_model, offered_version)) return false;
  return otaImageHasIdentity(image, n, id);
}
}  // namespace pc
