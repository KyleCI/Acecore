#pragma once

#include "factory_station.h"
#include "time_policy.h"

#include <stddef.h>
#include <stdint.h>

namespace powercell {

struct CloudState {
  char owner[65];
  bool claimed = false;
  bool hello_due = true;
};

// status "bound" and a valid owner id stores the owner. Anything else is ignored.
bool claim_result(CloudState* state, const char* status, const char* owner_id);

void device_hello(const CloudState& state, const char* display_id, bool passcode_default, char* out, size_t cap);

// METHOD \n PATH \n timestamp \n nonce \n hex(sha256(body)), HMAC-SHA256 with the device secret.
bool ota_signature(const uint8_t secret[32], const char* method, const char* path, const char* timestamp,
                   const char* nonce, const char* body, char sig_hex[65]);

// Bearer is optional. A missing secret or an untrusted clock (not phone/NTP) does not send.
struct OtaCheckInput {
  const char* mac_hex = nullptr;
  const char* serial = nullptr;
  const char* version = nullptr;
  const char* kind = nullptr;
  const char* model = nullptr;
  bool healthy = false;
  const char* last_status = nullptr;
  const char* last_error = nullptr;
  const char* bearer = nullptr;
  const uint8_t* secret = nullptr;
  bool have_secret = false;
  int64_t unix_s = 0;
  int clock_source = 0;
  const char* nonce_hex = nullptr;
};

struct OtaCheckRequest {
  bool send = false;
  char reason[32];
  char path[48];
  char body[480];
  char authorization[160];
  bool bearer_present = false;
  char device_header[24];
  char timestamp[24];
  char nonce[33];
  char signature[65];
};

struct OtaSigned {
  bool send = false;
  char reason[32];
  char authorization[160];
  bool bearer_present = false;
  char device_header[24];
  char timestamp[24];
  char nonce[33];
  char signature[65];
};

bool sign_ota_headers(const char* method, const char* path, const char* body, const char* mac, const char* serial,
                      const char* bearer, const uint8_t* secret, bool have_secret, int64_t unix_s, int clock_source,
                      const char* nonce, OtaSigned* out);
bool build_ota_check(const OtaCheckInput& in, OtaCheckRequest* out);
bool ota_may_check(bool healthy, bool running, bool have_base_url, bool wifi_up, bool hotspot_only,
                   bool have_secret, bool clock_trusted);

struct OtaSchedule {
  uint32_t next_ms = 0;
  bool armed = false;
  void begin(uint32_t now_ms) {
    next_ms = now_ms + 35000;
    armed = true;
  }
  // True once, then the next check is period_ms later. No check without a signature,
  // Wi-Fi, or while the unit is hotspot-only.
  bool due(uint32_t now_ms, bool wifi_up, bool hotspot_only, bool can_sign, uint32_t period_ms) {
    if (!armed || !wifi_up || hotspot_only || !can_sign) return false;
    if ((int32_t)(now_ms - next_ms) < 0) return false;
    next_ms = now_ms + period_ms;
    return true;
  }
};

struct OutputMap {
  bool have_ac = false;
  float output_voltage = 0;
  float output_current = 0;
  float output_power_apparent_va = 0;
  float output_power_w = 0;
  bool have_bms = false;
  float battery_voltage = 0;
  float battery_current_a = 0;
  float battery_power_w = 0;
  float remaining_ah = 0;
  int cycle_count = 0;
  int soh = 0;
  bool have_pv = false;
  float solar_voltage = 0;
  float solar_current = 0;
  float solar_power_w = 0;
};

OutputMap map_outputs(bool pzem_fresh, float volts, float amps, float active_w, bool bms_fresh, float bat_v,
                      float bat_a, float bat_w, float remain_ah, int cycles, int soh, bool pv_fresh, float pv_v,
                      float pv_a, float pv_w);

}  // namespace powercell
