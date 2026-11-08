#pragma once

#include <stddef.h>
#include <stdint.h>

// Token store copied from deployed collector 2.5.11 (4f39c95) onboarding:
// namespace pc_onb, passcode verifier "pass_v", pair blob "pairs".
// A bearer value is a 48-hex session (30 min, RAM) or the phone's LAN token
// (SHA-256 in the pair blob). Login replaces that phone's previous token.
namespace powercell {

enum class LanKind : uint8_t { None = 0, Session, Lan };

struct DeviceNames {
  const char* requested;
  const char* mac_colon;       // "AA:BB:CC:DD:EE:FF", may be null
  const char* collector_ap;    // this firmware's softAP SSID
  const char* stored_gw_name;  // 2.5.11 pc_secure/gw_name, may be null
  const char* stored_serial;   // 2.5.11 pc_ident/serial, may be null
};

// Same accepts as 2.5.11 matchesDeviceId, plus this collector's own AP SSID.
bool login_device_matches(const DeviceNames& names);

enum class LoginStatus : uint8_t { Ok = 0, BadRequest, BadCredentials, Locked, StorageFailed };

class LanAccess {
 public:
  LanAccess();
  void clear();
  bool load_verifier_hex(const char* hex96);
  bool has_passcode() const { return has_passcode_; }
  bool passcode_default() const { return passcode_default_; }
  // Exact pair-blob size. A different size is rejected and the store stays empty.
  bool load_pairs(const void* bytes, size_t len);
  size_t pairs_size() const;
  void export_pairs(void* out) const;

  LanKind authorize(const char* header, uint32_t now_ms) const;

  // device_ok is the caller's matchesDeviceId result. A mismatch is bad credentials.
  LoginStatus login(bool device_ok, const char* passcode, const char* phone, const char* name,
                    uint32_t now_ms, uint32_t unix_s, uint32_t client_key,
                    char session[49], char lan[49]);
  // First passcode only. Salt is 16 bytes from the caller (firmware: esp_fill_random).
  LoginStatus setup_passcode(const char* passcode, const char* phone, const char* name,
                             const uint8_t salt[16], uint32_t now_ms, uint32_t unix_s,
                             char session[49], char lan[49]);
  // Persist hook. Login and setup fail with StorageFailed when this returns false.
  void set_persist(bool (*fn)(void* ctx, const void* pairs, size_t pairs_len, const char* verifier_hex),
                   void* ctx);
  void set_random(void (*fn)(uint8_t* out, size_t n, void* ctx), void* ctx);

 private:
  struct State;
  State* state_;
  bool has_passcode_ = false;
  bool passcode_default_ = false;
  bool (*persist_)(void*, const void*, size_t, const char*) = nullptr;
  void* persist_ctx_ = nullptr;
  bool persist_now(const char* verifier_hex);
};

}  // namespace powercell
