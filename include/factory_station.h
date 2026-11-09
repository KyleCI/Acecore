#pragma once

#include "display_identity.h"
#include "payment_lock.h"

#include <stddef.h>
#include <stdint.h>

// Host-testable factory console and app Wi-Fi session.
// The device secret is never copied into INFO text.
namespace powercell {

struct FactoryStation {
  char serial[16] = {};
  char mac_hex[13] = {};
  uint8_t secret[32];
  bool secret_set = false;
  bool secret_reported = false;
  bool locked = false;
  char ap_pass[16] = {};
  bool api_key_set = false;
  bool ota_url_set = false;
  char wifi_ssid[33] = {};
  bool payment_locked = false;
  char payment_reason[65] = {};
  uint32_t payment_last = 0;
  char payment_ids[8][65];
  uint8_t payment_next = 0;
  int usable_w = 0;
  bool reset_armed = false;

  void set_secret(const uint8_t bytes[32]);
  bool take_secret_once(char hex[65]);
  bool payment_seen(const char* cmd_id) const;
  // Returns false when the signature or verdict does not apply. Does not clear the lock on failure.
  bool apply_payment(const char* cmd_id, bool locked, const char* reason, uint32_t issued_at,
                     const char* sig_hex);
  void customer_reset();
  // One console line. Reply is written to out. Secret values are not echoed except SECRET once.
  void console(const char* line, char* out, size_t cap);
};

struct WifiSession {
  char test_id[9];
  char ssid[33];
  char pass[64];
  char ip[16];
  char saved_ssid[33];
  char saved_pass[64];
  int state = 0;  // 0 idle, 1 testing, 2 ok, 3 failed
  char mode[16];

  bool start(const char* ssid, const char* pass, const char* id);
  void succeed(const char* ip);
  bool save(char* saved_ip, size_t cap);
};

struct AcPublication {
  bool publish_active = false;
  float voltage_v = 0;
  float current_a = 0;
  float active_w = 0;
  float energy_wh = 0;
  bool grid_voltage = false;
  bool temperature = false;
  bool apparent_power = false;
  bool pv = false;
};

// PZEM may fill only the AC channels it actually measures. STM32, grid, PV stay absent.
AcPublication publish_pzem(bool valid, float voltage_v, float current_a, float active_w, float energy_wh);

void ota_check_json(const char* mac_hex, const char* version, const char* kind, const char* model,
                    bool healthy, const char* last_status, const char* last_error, char* out, size_t cap);
void ota_download_path(const char* mac_hex, char* out, size_t cap);

}  // namespace powercell
