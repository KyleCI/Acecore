#pragma once
#include <stddef.h>
#include <stdint.h>

// Portable SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 (FIPS 180-4, RFC 2104,
// RFC 8018). Host-tested against the published vectors. Used for the
// onboarding passcode verifier, fingerprint, claim hash and LAN token hashes.
namespace pc {
class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void *data, size_t len);
  void finish(uint8_t out[32]);
  uint32_t h_[8];
  uint64_t bits_;
  uint8_t buf_[64];
  size_t n_;
 private:
  void block(const uint8_t *p);
};
void sha256(const void *data, size_t len, uint8_t out[32]);
void hmacSha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
extern void (*g_pbkdf2Yield)();  // called every 256 rounds (firmware: vTaskDelay(1))
void pbkdf2Sha256(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint32_t iters, uint8_t *out, size_t outLen);
// Lowercase hex of n bytes into out (needs 2n+1).
void toHex(const uint8_t *b, size_t n, char *out);
bool fromHex(const char *s, uint8_t *out, size_t n);  // exactly 2n hex chars
}  // namespace pc
