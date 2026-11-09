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

bool ota_may_check(bool healthy, bool running, bool have_base_url, bool wifi_up, bool hotspot_only,
                   bool have_secret, bool clock_trusted) {
  return healthy && !running && have_base_url && wifi_up && !hotspot_only && have_secret && clock_trusted;
}

bool sign_ota_headers(const char* method, const char* path, const char* body, const char* mac, const char* serial,
                      const char* bearer, const uint8_t* secret, bool have_secret, int64_t unix_s, int clock_source,
                      const char* nonce, OtaSigned* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  snprintf(out->reason, sizeof out->reason, "unsigned_refused");
  const bool phone_or_ntp = clock_source == 2 || clock_source == 3;
  const bool nonce_ok = nonce && strlen(nonce) == 32;
  if (!method || !path || !have_secret || !secret || !phone_or_ntp || unix_s < pc::clk::kMinEpoch || !nonce_ok)
    return false;
  if (factory_serial_valid(serial)) snprintf(out->device_header, sizeof out->device_header, "%s", serial);
  else snprintf(out->device_header, sizeof out->device_header, "%s", mac ? mac : "");
  snprintf(out->timestamp, sizeof out->timestamp, "%lld", static_cast<long long>(unix_s));
  snprintf(out->nonce, sizeof out->nonce, "%s", nonce);
  if (bearer && bearer[0]) {
    snprintf(out->authorization, sizeof out->authorization, "Bearer %s", bearer);
    out->bearer_present = true;
  }
  if (!ota_signature(secret, method, path, out->timestamp, out->nonce, body ? body : "", out->signature)) return false;
  out->send = true;
  snprintf(out->reason, sizeof out->reason, "signed");
  return true;
}

bool build_ota_check(const OtaCheckInput& in, OtaCheckRequest* out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  snprintf(out->path, sizeof out->path, "/api/ota/check");
  ota_check_json(in.mac_hex, in.version, in.kind, in.model, in.healthy, in.last_status, in.last_error, out->body,
                 sizeof out->body);
  OtaSigned signed_headers;
  const bool ok = sign_ota_headers("POST", out->path, out->body, in.mac_hex, in.serial, in.bearer, in.secret,
                                   in.have_secret, in.unix_s, in.clock_source, in.nonce_hex, &signed_headers);
  snprintf(out->reason, sizeof out->reason, "%s", signed_headers.reason);
  snprintf(out->authorization, sizeof out->authorization, "%s", signed_headers.authorization);
  out->bearer_present = signed_headers.bearer_present;
  snprintf(out->device_header, sizeof out->device_header, "%s", signed_headers.device_header);
  snprintf(out->timestamp, sizeof out->timestamp, "%s", signed_headers.timestamp);
  snprintf(out->nonce, sizeof out->nonce, "%s", signed_headers.nonce);
  snprintf(out->signature, sizeof out->signature, "%s", signed_headers.signature);
  out->send = ok && signed_headers.send;
  return out->send;
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

int powercell_ota_may_check(int healthy, int running, int have_base_url, int wifi_up, int hotspot_only, int have_secret,
                            int clock_trusted) {
  return powercell::ota_may_check(healthy, running, have_base_url, wifi_up, hotspot_only, have_secret, clock_trusted)
             ? 1
             : 0;
}

int powercell_prepare_ota_check(const char* mac, const char* serial, const char* version, const char* kind,
                                const char* model, int healthy, const char* last_status, const char* last_error,
                                const char* bearer, const uint8_t* secret, int have_secret, long long unix_s,
                                int clock_source, const char* nonce, char* body, size_t body_cap, char* authorization,
                                size_t auth_cap, char* device_header, size_t device_cap, char* timestamp, size_t ts_cap,
                                char* signature, size_t sig_cap, char* reason, size_t reason_cap) {
  powercell::OtaCheckInput in;
  in.mac_hex = mac;
  in.serial = serial;
  in.version = version;
  in.kind = kind;
  in.model = model;
  in.healthy = healthy != 0;
  in.last_status = last_status;
  in.last_error = last_error;
  in.bearer = bearer;
  in.secret = secret;
  in.have_secret = have_secret != 0;
  in.unix_s = unix_s;
  in.clock_source = clock_source;
  in.nonce_hex = nonce;
  powercell::OtaCheckRequest req;
  powercell::build_ota_check(in, &req);
  if (body && body_cap) snprintf(body, body_cap, "%s", req.body);
  if (authorization && auth_cap) snprintf(authorization, auth_cap, "%s", req.authorization);
  if (device_header && device_cap) snprintf(device_header, device_cap, "%s", req.device_header);
  if (timestamp && ts_cap) snprintf(timestamp, ts_cap, "%s", req.timestamp);
  if (signature && sig_cap) snprintf(signature, sig_cap, "%s", req.signature);
  if (reason && reason_cap) snprintf(reason, reason_cap, "%s", req.reason);
  return req.send ? 1 : 0;
}

int powercell_sign_ota(const char* method, const char* path, const char* body, const char* mac, const char* serial,
                      const char* bearer, const uint8_t* secret, int have_secret, long long unix_s, int clock_source,
                      const char* nonce, char* authorization, size_t auth_cap, char* device_header, size_t device_cap,
                      char* timestamp, size_t ts_cap, char* signature, size_t sig_cap, char* reason, size_t reason_cap) {
  powercell::OtaSigned signed_headers;
  powercell::sign_ota_headers(method, path, body, mac, serial, bearer, secret, have_secret != 0, unix_s, clock_source,
                              nonce, &signed_headers);
  if (authorization && auth_cap) snprintf(authorization, auth_cap, "%s", signed_headers.authorization);
  if (device_header && device_cap) snprintf(device_header, device_cap, "%s", signed_headers.device_header);
  if (timestamp && ts_cap) snprintf(timestamp, ts_cap, "%s", signed_headers.timestamp);
  if (signature && sig_cap) snprintf(signature, sig_cap, "%s", signed_headers.signature);
  if (reason && reason_cap) snprintf(reason, reason_cap, "%s", signed_headers.reason);
  return signed_headers.send ? 1 : 0;
}

bool powercell_factory_serial(const char* serial) { return factory_serial_valid(serial); }
