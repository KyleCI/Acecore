#include "factory_station.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

namespace powercell {
namespace {
void hex32(const uint8_t in[32], char out[65]) {
  static const char* h = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = h[in[i] >> 4];
    out[i * 2 + 1] = h[in[i] & 15];
  }
  out[64] = 0;
}
bool hex_equal(const char* a, const char* b) {
  if (!a || !b || strlen(a) != 64 || strlen(b) != 64) return false;
  for (int i = 0; i < 64; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'F') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'F') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}
const char* arg_after(const char* line, const char* key) {
  size_t n = strlen(key);
  if (strncmp(line, key, n) != 0 || line[n] != ' ') return nullptr;
  return line + n + 1;
}
}  // namespace

void FactoryStation::set_secret(const uint8_t bytes[32]) {
  memcpy(secret, bytes, 32);
  secret_set = true;
  secret_reported = false;
}

bool FactoryStation::take_secret_once(char hex[65]) {
  if (!secret_set || secret_reported || locked) return false;
  secret_reported = true;
  hex32(secret, hex);
  return true;
}

bool FactoryStation::payment_seen(const char* cmd_id) const {
  if (!cmd_id || !cmd_id[0]) return false;
  for (int i = 0; i < 8; ++i) {
    if (payment_ids[i][0] && strcmp(payment_ids[i], cmd_id) == 0) return true;
  }
  return false;
}

bool FactoryStation::apply_payment(const char* cmd_id, bool lock, const char* reason, uint32_t issued_at,
                                   const char* sig_hex) {
  char msg[160];
  size_t n = pc::paylock::message(cmd_id, lock, reason ? reason : "", issued_at, msg, sizeof msg);
  if (!n || !secret_set || !sig_hex) return false;
  uint8_t mac[32];
  pc::hmacSha256(secret, 32, msg, n, mac);
  char hex[65];
  hex32(mac, hex);
  if (!hex_equal(hex, sig_hex)) return false;
  pc::paylock::Verdict verdict = pc::paylock::judge(issued_at, payment_last, payment_seen(cmd_id));
  if (verdict != pc::paylock::Verdict::Apply) return false;
  payment_locked = lock;
  snprintf(payment_reason, sizeof payment_reason, "%s", lock && reason ? reason : "");
  payment_last = issued_at;
  snprintf(payment_ids[payment_next % 8], 65, "%s", cmd_id);
  payment_next = (uint8_t)((payment_next + 1) % 8);
  return true;
}

void FactoryStation::customer_reset() {
  wifi_ssid[0] = 0;
  reset_armed = false;
}

void FactoryStation::console(const char* line, char* out, size_t cap) {
  if (!line || !out || !cap) return;
  out[0] = 0;
  if (strcmp(line, "INFO") == 0) {
    char ap[24];
    label_hotspot(serial, ap);
    snprintf(out, cap,
             "device_id=%s\nserial=%s ap_ssid=%s secret=%s secret_reported=%d locked=%d api_key=%s\n"
             "model=%s usable_kw=%s rated_kva=null\n",
             mac_hex[0] ? mac_hex : "unset", factory_serial_valid(serial) ? serial : "unset",
             ap[0] ? ap : "unset", secret_set ? "set" : "MISSING", secret_reported ? 1 : 0, locked ? 1 : 0,
             api_key_set ? "set" : "MISSING", usable_w == 4500 ? "PC5K" : "unset",
             usable_w == 4500 ? "4.5" : usable_w == 3150 ? "3.15" : "unset");
    if (!locked && factory_serial_valid(serial) && ap_pass[0]) {
      size_t used = strlen(out);
      snprintf(out + used, cap - used, "LABEL PWC2;S=%s;P=%s;D=%s\nLABEL_QR WIFI:T:WPA;S:%s;P:%s;;\n", ap,
               ap_pass, serial, ap, ap_pass);
    }
    return;
  }
  if (strcmp(line, "SECRET") == 0) {
    char hex[65];
    if (!take_secret_once(hex)) {
      snprintf(out, cap, "%s", secret_reported ? "ERR already reported" : "ERR no secret");
      return;
    }
    snprintf(out, cap, "SECRET %s", hex);
    return;
  }
  if (strcmp(line, "ROTATE_SECRET") == 0) {
    snprintf(out, cap, "%s", locked ? "ERR locked" : "OK new secret");
    if (!locked) secret_reported = false;
    return;
  }
  if (strcmp(line, "LOCK") == 0) {
    if (!factory_serial_valid(serial) || !secret_reported) {
      snprintf(out, cap, "ERR lock requires serial and a reported secret");
      return;
    }
    locked = true;
    snprintf(out, cap, "OK locked");
    return;
  }
  if (strncmp(line, "SERIAL ", 7) == 0) {
    if (locked) {
      snprintf(out, cap, "ERR locked");
      return;
    }
    const char* s = line + 7;
    if (!factory_serial_valid(s)) {
      snprintf(out, cap, "ERR serial must be ACE-NNNNNN");
      return;
    }
    snprintf(serial, sizeof serial, "%s", s);
    snprintf(out, cap, "OK serial set");
    return;
  }
  if (strcmp(line, "FACTORY_RESET") == 0) {
    if (!reset_armed) {
      reset_armed = true;
      snprintf(out, cap, "ERR type FACTORY_RESET again");
      return;
    }
    bool kept = payment_locked;
    char kept_serial[16];
    snprintf(kept_serial, sizeof kept_serial, "%s", serial);
    customer_reset();
    payment_locked = kept;
    snprintf(serial, sizeof serial, "%s", kept_serial);
    snprintf(out, cap, "OK reset identity kept payment_locked=%d", payment_locked ? 1 : 0);
    return;
  }
  const char* model = arg_after(line, "SET model");
  if (model) {
    if (strcmp(model, "PC3K") == 0 || strcmp(model, "PC10K") == 0) {
      snprintf(out, cap, "ERR model does not match the flashed usable watts");
      return;
    }
    if (strcmp(model, "PC5K") == 0 && usable_w == 4500) {
      snprintf(out, cap, "OK model=PC5K usable_kw=4.5");
      return;
    }
    snprintf(out, cap, "ERR model does not match the flashed usable watts");
    return;
  }
  const char* key = arg_after(line, "SET api_key");
  if (key) {
    if (locked) {
      snprintf(out, cap, "ERR locked");
      return;
    }
    api_key_set = key[0] != 0;
    snprintf(out, cap, "OK api_key");
    return;
  }
  const char* url = arg_after(line, "SET ota_url");
  if (url) {
    if (locked) {
      snprintf(out, cap, "ERR locked");
      return;
    }
    ota_url_set = strncmp(url, "https://", 8) == 0;
    snprintf(out, cap, "%s", ota_url_set ? "OK ota_url" : "ERR ota_url");
    return;
  }
  const char* ssid = arg_after(line, "SET wifi_ssid");
  if (ssid) {
    snprintf(wifi_ssid, sizeof wifi_ssid, "%s", ssid);
    snprintf(out, cap, "OK wifi_ssid");
    return;
  }
  snprintf(out, cap, "ERR unsupported");
}

bool WifiSession::start(const char* name, const char* password, const char* id) {
  if (!name || !name[0] || strlen(name) > 32 || (password && strlen(password) > 63) || state == 1) return false;
  snprintf(ssid, sizeof ssid, "%s", name);
  snprintf(pass, sizeof pass, "%s", password ? password : "");
  snprintf(test_id, sizeof test_id, "%s", id ? id : "abcd0123");
  ip[0] = 0;
  state = 1;
  return true;
}

void WifiSession::succeed(const char* address) {
  snprintf(ip, sizeof ip, "%s", address ? address : "");
  state = 2;
}

bool WifiSession::save(char* saved_ip, size_t cap) {
  if (state != 2 || !ip[0]) return false;
  snprintf(saved_ssid, sizeof saved_ssid, "%s", ssid);
  snprintf(saved_pass, sizeof saved_pass, "%s", pass);
  snprintf(saved_ip, cap, "%s", ip);
  state = 0;
  pass[0] = 0;
  snprintf(mode, sizeof mode, "station");
  return true;
}

AcPublication publish_pzem(bool valid, float voltage_v, float current_a, float active_w, float energy_wh) {
  AcPublication out;
  if (!valid) return out;
  out.publish_active = true;
  out.voltage_v = voltage_v;
  out.current_a = current_a;
  out.active_w = active_w;
  out.energy_wh = energy_wh;
  return out;
}

void ota_check_json(const char* mac, const char* version, const char* kind, const char* model, bool healthy,
                    const char* last_status, const char* last_error, char* out, size_t cap) {
  snprintf(out, cap,
           "{\"device_id\":\"%s\",\"firmware_version\":\"%s\",\"kind\":\"%s\",\"model\":\"%s\","
           "\"healthy\":%s,\"last_status\":\"%s\",\"last_error\":\"%s\"}",
           mac ? mac : "", version ? version : "", kind ? kind : "collector", model ? model : "",
           healthy ? "true" : "false", last_status ? last_status : "", last_error ? last_error : "");
}

void ota_download_path(const char* mac, char* out, size_t cap) {
  snprintf(out, cap, "/api/ota/download?device_id=%s", mac ? mac : "");
}

}  // namespace powercell
