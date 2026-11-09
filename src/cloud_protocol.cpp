#include "cloud_protocol.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

namespace powercell {
namespace {
bool owner_ok(const char* owner) {
  size_t n = owner ? strlen(owner) : 0;
  if (!n || n > 64) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)owner[i];
    if (c < 0x21 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}
void hex_of(const uint8_t* in, size_t n, char* out) {
  static const char* h = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[i * 2] = h[in[i] >> 4];
    out[i * 2 + 1] = h[in[i] & 15];
  }
  out[n * 2] = 0;
}
}  // namespace

bool claim_result(CloudState* state, const char* status, const char* owner_id) {
  if (!state || !status || strcmp(status, "bound") != 0 || !owner_ok(owner_id)) return false;
  snprintf(state->owner, sizeof state->owner, "%s", owner_id);
  state->claimed = true;
  state->hello_due = true;
  return true;
}

void device_hello(const CloudState& state, const char* display_id, bool passcode_default, char* out, size_t cap) {
  snprintf(out, cap,
           "{\"type\":\"device_hello\",\"onboarding_version\":2,\"display_id\":\"%s\","
           "\"passcode\":{\"is_default\":%s},\"owner_id\":%s%s%s}",
           display_id ? display_id : "", passcode_default ? "true" : "false",
           state.claimed ? "\"" : "", state.claimed ? state.owner : "null", state.claimed ? "\"" : "");
}

bool ota_signature(const uint8_t secret[32], const char* method, const char* path, const char* timestamp,
                   const char* nonce, const char* body, char sig_hex[65]) {
  if (!secret || !method || !path || !timestamp || !nonce || !sig_hex) return false;
  uint8_t digest[32];
  pc::sha256(body ? body : "", body ? strlen(body) : 0, digest);
  char body_hex[65];
  hex_of(digest, 32, body_hex);
  char msg[640];
  int n = snprintf(msg, sizeof msg, "%s\n%s\n%s\n%s\n%s", method, path, timestamp, nonce, body_hex);
  if (n <= 0 || n >= (int)sizeof msg) return false;
  uint8_t mac[32];
  pc::hmacSha256(secret, 32, msg, (size_t)n, mac);
  hex_of(mac, 32, sig_hex);
  return true;
}

OutputMap map_outputs(bool pzem_fresh, float volts, float amps, float active_w, bool bms_fresh, float bat_v,
                      float bat_a, float bat_w, float remain_ah, int cycles, int soh, bool pv_fresh, float pv_v,
                      float pv_a, float pv_w) {
  OutputMap out;
  if (pzem_fresh) {
    out.have_ac = true;
    out.output_voltage = volts;
    out.output_current = amps;
    out.output_power_apparent_va = volts * amps;
    out.output_power_w = active_w;
  }
  if (bms_fresh) {
    out.have_bms = true;
    out.battery_voltage = bat_v;
    out.battery_current_a = bat_a;
    out.battery_power_w = bat_w;
    out.remaining_ah = remain_ah;
    out.cycle_count = cycles;
    out.soh = soh;
  }
  if (pv_fresh) {
    out.have_pv = true;
    out.solar_voltage = pv_v;
    out.solar_current = pv_a;
    out.solar_power_w = pv_w;
  }
  return out;
}

}  // namespace powercell

bool powercell_ota_signature(const uint8_t secret[32], const char* method, const char* path, const char* timestamp,
                             const char* nonce, const char* body, char sig_hex[65]) {
  return powercell::ota_signature(secret, method, path, timestamp, nonce, body, sig_hex);
}

bool powercell_factory_serial(const char* serial) { return factory_serial_valid(serial); }
