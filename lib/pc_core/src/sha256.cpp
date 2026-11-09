#include "sha256.h"
#include <string.h>

namespace pc {
namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

void Sha256::reset() {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h_, iv, sizeof h_);
  bits_ = 0; n_ = 0;
}
void Sha256::block(const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}
void Sha256::update(const void *data, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  bits_ += uint64_t(len) * 8;
  while (len) {
    size_t take = 64 - n_ < len ? 64 - n_ : len;
    memcpy(buf_ + n_, p, take); n_ += take; p += take; len -= take;
    if (n_ == 64) { block(buf_); n_ = 0; }
  }
}
void Sha256::finish(uint8_t out[32]) {
  uint64_t bits = bits_;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t z = 0;
  while (n_ != 56) update(&z, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; ++i) { out[4 * i] = h_[i] >> 24; out[4 * i + 1] = h_[i] >> 16; out[4 * i + 2] = h_[i] >> 8; out[4 * i + 3] = h_[i]; }
}
void sha256(const void *data, size_t len, uint8_t out[32]) { Sha256 s; s.update(data, len); s.finish(out); }

namespace {
struct Hmac {
  Sha256 in, out;
  Hmac(const uint8_t *key, size_t klen) {
    uint8_t k[64] = {0}, pad[64];
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    in.update(pad, 64);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    out.update(pad, 64);
  }
  // Keyed states are copied, so PBKDF2 costs two compressions per round.
  void mac(const void *m, size_t n, uint8_t r[32]) const {
    Sha256 i = in, o = out; uint8_t t[32];
    i.update(m, n); i.finish(t); o.update(t, 32); o.finish(r);
  }
};
}  // namespace

void hmacSha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]) { Hmac(key, klen).mac(msg, mlen, out); }

void (*g_pbkdf2Yield)() = nullptr;
void pbkdf2Sha256(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint32_t iters, uint8_t *out, size_t outLen) {
  Hmac h(pass, plen);
  for (uint32_t blk = 1; outLen; ++blk) {
    uint8_t u[32], t[32];
    Sha256 i = h.in, o = h.out; uint8_t c[4] = {uint8_t(blk >> 24), uint8_t(blk >> 16), uint8_t(blk >> 8), uint8_t(blk)};
    i.update(salt, slen); i.update(c, 4); i.finish(u); o.update(u, 32); o.finish(u);
    memcpy(t, u, 32);
    for (uint32_t r = 1; r < iters; ++r) { h.mac(u, 32, u); for (int k = 0; k < 32; ++k) t[k] ^= u[k]; if (g_pbkdf2Yield && !(r & 255)) g_pbkdf2Yield(); }
    size_t n = outLen < 32 ? outLen : 32;
    memcpy(out, t, n); out += n; outLen -= n;
  }
}

void toHex(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
  out[2 * n] = 0;
}
bool fromHex(const char *s, uint8_t *out, size_t n) {
  if (!s || strlen(s) != 2 * n) return false;
  auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (size_t i = 0; i < n; ++i) {
    int a = v(s[2 * i]), b = v(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = uint8_t(a << 4 | b);
  }
  return true;
}
}  // namespace pc
