#pragma once

#include "ac_output_settings.h"
#include "battery_settings.h"
#include "board_pins.h"
#include "lan_access.h"

#include <stddef.h>
#include <stdint.h>

namespace powercell {

// Device HTTP decisions shared by firmware routes and host tests.
// Login matches 2.5.11 POST /v2/login. Settings are a new local contract:
// app PR 8 build 73 has no local settings client.
struct HttpReply {
  int status = 500;
  char body[1400];
};

struct ClientHeaders {
  bool content_type_json = false;
  bool accept_json = true;
};

struct UnitFacts {
  bool wifi_configured = false;
  bool hotspot = false;
  bool sta_connected = false;
  bool payment_locked = false;
  const char* model = "esp32dev";
  const char* firmware = "1.0.1";
};

enum class OutputProfile : uint8_t { OriginalRelay = 0, V011Dsp = 1 };

struct RouteEffect {
  HttpReply http;
  bool drive_gateway = false;
  bool gateway_on = false;
  bool queue_aceplug = false;
  bool aceplug_on = false;
  char socket_id[40];
};

struct ManualImage {
  char verifier_hex[97];
  bool has_passcode = false;
  uint8_t pairs[1400];
  size_t pairs_len = 0;
  int low_percent = 25;
  int warning_percent = 15;
  uint32_t battery_revision = 0;
  int ceiling_w = 0;
  int ac_mode = 0;
  uint32_t ac_revision = 0;
  bool rating_store_present = false;
  int stored_rated_active_w = 0;
};

class ManualApi {
 public:
  explicit ManualApi(int factory_rated_w = 0, const char* factory_variant = "unrated");
  void set_random(void (*fn)(uint8_t*, size_t, void*), void* ctx);
  void set_persist_fail(bool fail);
  void capture(ManualImage* out) const;
  bool restore(const ManualImage& in);

  HttpReply setup_info(const DeviceNames& device, const UnitFacts& facts);
  HttpReply login(const char* json, const ClientHeaders& headers, const DeviceNames& device,
                  uint32_t now_ms, uint32_t unix_s, uint32_t client_key);
  HttpReply setup_passcode(const char* json, const ClientHeaders& headers, bool on_hotspot,
                           const uint8_t salt[16], uint32_t now_ms, uint32_t unix_s);
  // Immediate /v2/status: session if that bearer is a live session, otherwise the LAN token.
  HttpReply status(const char* authorization, const DeviceNames& device, const UnitFacts& facts,
                   uint32_t now_ms);
  // Saved-LAN routes. A live session alone is not enough. OTA and API-KEY are rejected.
  HttpReply battery_get(const char* authorization, uint32_t now_ms);
  HttpReply battery_post(const char* authorization, const char* json, const ClientHeaders& headers,
                         uint32_t now_ms);
  HttpReply ac_get(const char* authorization, uint32_t now_ms);
  HttpReply ac_post(const char* authorization, const char* json, const ClientHeaders& headers,
                    uint32_t now_ms);
  HttpReply rating_get(const char* authorization, uint32_t now_ms);
  HttpReply rating_post(const char* authorization, const char* json, const ClientHeaders& headers,
                        uint32_t now_ms);
  // /data: no header stays open for the on-device page. A presented bearer must be the LAN token.
  int data_auth(const char* authorization, uint32_t now_ms) const;
  // /state is the collector output. v0.1.1 is GPIO15 active-low. It is not an AcePlug.
  RouteEffect gateway_state(const char* authorization, const char* json, const ClientHeaders& headers,
                            OutputProfile profile, bool output_on_allowed, bool ota_busy, bool payment_locked,
                            uint32_t now_ms);
  // /relay is an AcePlug socket over LoRa. v0.1.1 has no LoRa and does not touch GPIO15.
  RouteEffect socket_relay(const char* authorization, const char* json, const ClientHeaders& headers,
                           bool lora_fitted, bool socket_registered, bool output_on_allowed, bool payment_locked,
                           uint32_t now_ms);

  const BatterySettings& battery() const { return battery_; }
  const AcOutputSettings& ac() const { return ac_; }
  const SystemRating& rating() const { return rating_; }

 private:
  LanAccess lan_;
  BatterySettings battery_;
  AcOutputSettings ac_;
  SystemRating rating_;
  char verifier_hex_[97];
  bool fail_persist_ = false;
  bool persist(void* ctx, const void* pairs, size_t len, const char* verifier_hex);
  static bool persist_thunk(void* ctx, const void* pairs, size_t len, const char* verifier_hex);
  LanKind bearer_kind(const char* authorization, uint32_t now_ms) const;
};

}  // namespace powercell
