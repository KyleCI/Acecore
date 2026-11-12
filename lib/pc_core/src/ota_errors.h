#pragma once
// 2.5.18: specific cloud OTA failure codes (replace the catch-all
// download_or_verification_failed) and the effective OTA host for the hello.
// Host-testable: no Arduino types.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

namespace pc {
namespace otaerr {

enum class Fail : uint8_t {
  None,
  NoSlot,           // download_no_slot: no inactive OTA partition
  UrlInvalid,       // download_url_invalid: HTTPClient::begin refused base+path
  ConnectFailed,    // download_connect_failed: GET < 0 (DNS / TCP / TLS handshake / send)
  Timeout,          // download_timeout: no response headers in time (-11), or body over the total cap
  Http,             // download_http_<code>: any HTTP status other than 200
  SizeMismatch,     // download_size_mismatch: no Content-Length, < 288 bytes, or != offered size
  TooLarge,         // download_too_large: Content-Length > the OTA slot
  FlashBeginFailed, // download_flash_begin_failed: esp_ota_begin
  Stalled,          // download_stalled: no byte within the read timeout
  ReadError,        // download_read_error: stream read <= 0
  VerifyMagic,      // verify_magic: first byte is not 0xE9
  FlashWriteFailed, // flash_write_failed: esp_ota_write
  VerifySize,       // verify_size: fewer bytes than Content-Length
  VerifyIdentity,   // verify_identity: POWERCELL_FW|kind|model|version|END not found
  VerifySha,        // verify_sha: SHA-256 differs from the offer
  FinalizeFailed,   // finalize_failed: esp_ota_end or set_boot_partition
  NotCurrent,       // download_not_current (2.5.26): 409/410 not_current_offer, the server offers another image now
};

// HTTPClient codes used below (HTTPClient.h).
constexpr int kHttpReadTimeout = -11;

inline const char *name(Fail f) {
  switch (f) {
    case Fail::None: return "";
    case Fail::NoSlot: return "download_no_slot";
    case Fail::UrlInvalid: return "download_url_invalid";
    case Fail::ConnectFailed: return "download_connect_failed";
    case Fail::Timeout: return "download_timeout";
    case Fail::Http: return "download_http";
    case Fail::SizeMismatch: return "download_size_mismatch";
    case Fail::TooLarge: return "download_too_large";
    case Fail::FlashBeginFailed: return "download_flash_begin_failed";
    case Fail::Stalled: return "download_stalled";
    case Fail::ReadError: return "download_read_error";
    case Fail::VerifyMagic: return "verify_magic";
    case Fail::FlashWriteFailed: return "flash_write_failed";
    case Fail::VerifySize: return "verify_size";
    case Fail::VerifyIdentity: return "verify_identity";
    case Fail::VerifySha: return "verify_sha";
    case Fail::FinalizeFailed: return "finalize_failed";
    case Fail::NotCurrent: return "download_not_current";
  }
  return "download_failed";
}

// The error string stored in pc_ota/error and reported as last_error.
// Http becomes download_http_<code> (e.g. download_http_401).
inline void code(Fail f, int http, char *out, size_t cap) {
  if (!out || !cap) return;
  if (f == Fail::Http) snprintf(out, cap, "download_http_%d", http);
  else {
    const char *n = name(f);
    size_t i = 0;
    for (; n[i] && i + 1 < cap; ++i) out[i] = n[i];
    out[i] = 0;
  }
}

// Classify the GET result and Content-Length before any flash work.
// len: HTTPClient::getSize() (-1 = no Content-Length / chunked).
inline Fail classifyResponse(int http, long len, size_t slot, uint32_t expect) {
  if (http == kHttpReadTimeout) return Fail::Timeout;
  if (http < 0) return Fail::ConnectFailed;
  if (http == 409 || http == 410) return Fail::NotCurrent;  // 2.5.26: the requested version/sha256 is not the offer
  if (http != 200) return Fail::Http;
  if (len > 0 && static_cast<unsigned long>(len) > slot) return Fail::TooLarge;
  if (len < 288 || (expect && static_cast<unsigned long>(len) != expect)) return Fail::SizeMismatch;
  return Fail::None;
}

// After the body loop: first failing check wins (stream errors were set in the loop).
inline Fail classifyVerify(Fail loop, size_t received, size_t size, bool identity, bool shaOk) {
  if (loop != Fail::None) return loop;
  if (received != size) return Fail::VerifySize;
  if (!identity) return Fail::VerifyIdentity;
  if (!shaOk) return Fail::VerifySha;
  return Fail::None;
}

// scheme://host[:port] of an OTA base URL, lower-cased, with no userinfo, path,
// query or fragment (nothing secret can leak into the hello). false if unparsable.
inline bool hostOf(const char *url, char *out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = 0;
  if (!url) return false;
  const char *sep = strstr(url, "://");
  if (!sep || sep == url) return false;
  size_t sl = static_cast<size_t>(sep - url);
  for (size_t i = 0; i < sl; ++i) if (!isalpha(static_cast<unsigned char>(url[i]))) return false;
  const char *h = sep + 3;
  const char *end = h + strcspn(h, "/?#");
  const char *at = nullptr;
  for (const char *p = h; p < end; ++p) if (*p == '@') at = p;
  if (at) h = at + 1;
  if (h >= end) return false;
  for (const char *p = h; p < end; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
  }
  size_t need = sl + 3 + static_cast<size_t>(end - h);
  if (need + 1 > cap) return false;
  size_t n = 0;
  for (size_t i = 0; i < sl; ++i) out[n++] = static_cast<char>(tolower(static_cast<unsigned char>(url[i])));
  out[n++] = ':'; out[n++] = '/'; out[n++] = '/';
  for (const char *p = h; p < end; ++p) out[n++] = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
  out[n] = 0;
  return true;
}

// True when the effective base URL is not the compiled default (a saved ota_url
// is in use). Case-insensitive, trailing slashes ignored.
inline bool isOverride(const char *effective, const char *compiled) {
  if (!effective || !compiled) return effective != compiled;
  size_t a = strlen(effective), b = strlen(compiled);
  while (a && effective[a - 1] == '/') --a;
  while (b && compiled[b - 1] == '/') --b;
  return a != b || strncasecmp(effective, compiled, a) != 0;
}

}  // namespace otaerr
}  // namespace pc
