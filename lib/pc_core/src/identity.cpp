#include "identity.h"
#include <stdio.h>
#include <string.h>
#include "crc.h"

namespace pc {
void macColon(const uint8_t m[6], char out[18]) { snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }
void macHex(const uint8_t m[6], char out[13]) { snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }
void gatewayName(const uint8_t m[6], char out[16]) { snprintf(out, 16, "ACE-%02X%02X%02X", m[3], m[4], m[5]); }
void apSsid(const uint8_t m[6], char out[24]) { snprintf(out, 24, "Powercell-%02X%02X%02X", m[3], m[4], m[5]); }

void apPassword(const uint8_t m[6], const uint8_t *salt, size_t saltLen, char out[11]) {
  static const char kAlpha[] = "abcdefghjkmnpqrstuvwxyz23456789";  // 31 chars, no l/1/o/0/i
  uint32_t a = crc32_ieee(m, 6, 0x5EED1234u);
  if (salt && saltLen) a = crc32_ieee(salt, saltLen, a);
  uint32_t b = crc32_ieee(reinterpret_cast<const uint8_t *>(&a), 4, a ^ 0xA5A5A5A5u);
  uint64_t x = (static_cast<uint64_t>(a) << 32) | b;
  for (int i = 0; i < 10; ++i) { out[i] = kAlpha[x % 31]; x /= 31; }
  out[10] = 0;
}

size_t urlEncode(const char *in, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (; *in; ++in) {
    unsigned char c = static_cast<unsigned char>(*in);
    bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    size_t need = keep ? 1 : 3;
    if (o + need >= cap) break;
    if (keep) out[o++] = c;
    else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
  }
  if (cap) out[o < cap ? o : cap - 1] = 0;
  return o;
}
}  // namespace pc
