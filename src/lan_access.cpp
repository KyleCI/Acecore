#include "lan_access.h"

#include "onb_core.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace powercell {
namespace {
using pc::onb::Limiter;
using pc::onb::PairStore;
using pc::onb::Sessions;
using pc::onb::Verifier;
using pc::onb::checkPasscode;
using pc::onb::kMaxPhones;
using pc::onb::kPhoneIdMax;
using pc::onb::kPhoneNameMax;
using pc::onb::makeVerifier;
using pc::onb::tokenFromRandom;
using pc::onb::validPasscode;
using pc::onb::verifierHex;

bool eq_ci(const char* a, const char* b) {
  if (!a || !b || !a[0] || !b[0]) return false;
  while (*a && *b) {
    char x = *a >= 'a' && *a <= 'z' ? char(*a - 32) : *a;
    char y = *b >= 'a' && *b <= 'z' ? char(*b - 32) : *b;
    if (x != y) return false;
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

bool mac_canon(const char* s, char out[13]) {
  if (!s) return false;
  size_t n = strlen(s);
  if (n == 12) {
    for (size_t i = 0; i < 12; ++i) {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
      out[i] = c >= 'a' && c <= 'f' ? char(c - 32) : c;
    }
    out[12] = 0;
    return true;
  }
  if (n != 17) return false;
  char sep = s[2];
  if (sep != ':' && sep != '-') return false;
  size_t o = 0;
  for (size_t i = 0; i < 17; ++i) {
    if (i % 3 == 2) {
      if (s[i] != sep) return false;
      continue;
    }
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
    out[o++] = c >= 'a' && c <= 'f' ? char(c - 32) : c;
  }
  out[12] = 0;
  return true;
}

void names_from_mac(const char* mac_colon, char gw[16], char ap[24]) {
  gw[0] = ap[0] = 0;
  char hex[13];
  if (!mac_canon(mac_colon, hex)) return;
  uint8_t m[6];
  for (int i = 0; i < 6; ++i) {
    char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    m[i] = static_cast<uint8_t>(strtoul(b, nullptr, 16));
  }
  // Same formats as 2.5.11 identity.cpp gatewayName / apSsid. No AP password.
  snprintf(gw, 16, "ACE-%02X%02X%02X", m[3], m[4], m[5]);
  snprintf(ap, 24, "Powercell-%02X%02X%02X", m[3], m[4], m[5]);
}

struct IpSlot {
  uint32_t key = 0;
  uint32_t last = 0;
  Limiter limiter;
};
}  // namespace

bool login_device_matches(const DeviceNames& names) {
  const char* id = names.requested;
  if (!id || !id[0]) return false;
  if (eq_ci(id, names.collector_ap)) return true;
  if (eq_ci(id, names.stored_gw_name)) return true;
  char mac[13], other[13];
  if (mac_canon(id, mac) && mac_canon(names.mac_colon, other) && strcmp(mac, other) == 0) return true;
  char gw[16], ap[24];
  names_from_mac(names.mac_colon, gw, ap);
  if (eq_ci(id, gw) || eq_ci(id, ap)) return true;
  if (names.stored_serial && names.stored_serial[0]) {
    if (eq_ci(id, names.stored_serial)) return true;
    if (strlen(names.stored_serial) > 4) {
      char powered[40];
      snprintf(powered, sizeof powered, "Powercell-%s", names.stored_serial + 4);
      if (eq_ci(id, powered)) return true;
    }
  }
  return false;
}

using pc::onb::Limiter;
using pc::onb::PairStore;
using pc::onb::Sessions;
using pc::onb::Verifier;
using pc::onb::tokenFromRandom;

struct LanAccess::State {
  Verifier verifier{};
  PairStore pairs;
  Sessions sessions;
  Limiter global;
  IpSlot ips[8];
  void (*random_bytes)(uint8_t*, size_t, void*) = nullptr;
  void* random_ctx = nullptr;
  char verifier_hex[97] = {};

  void random_token(char out[49]) {
    uint8_t r[24];
    if (random_bytes) random_bytes(r, sizeof r, random_ctx);
    else memset(r, 0, sizeof r);
    tokenFromRandom(r, out);
  }
  IpSlot& slot(uint32_t key, uint32_t now) {
    IpSlot* lru = &ips[0];
    for (auto& x : ips) {
      if (x.key == key) {
        x.last = now;
        return x;
      }
      if (x.last < lru->last) lru = &x;
    }
    *lru = IpSlot{};
    lru->key = key;
    lru->last = now;
    return *lru;
  }
};

LanAccess::LanAccess() : state_(new State()) { state_->pairs.clear(); }

void LanAccess::clear() {
  auto rnd = state_->random_bytes;
  auto ctx = state_->random_ctx;
  State* fresh = new State();
  fresh->random_bytes = rnd;
  fresh->random_ctx = ctx;
  delete state_;
  state_ = fresh;
  has_passcode_ = false;
  passcode_default_ = false;
}

bool LanAccess::load_verifier_hex(const char* hex96) {
  if (!pc::onb::verifierFromHex(hex96, state_->verifier)) return false;
  verifierHex(state_->verifier, state_->verifier_hex);
  has_passcode_ = true;
  return true;
}

bool LanAccess::load_pairs(const void* bytes, size_t len) {
  if (!bytes || len != sizeof(PairStore)) {
    state_->pairs.clear();
    return false;
  }
  memcpy(&state_->pairs, bytes, sizeof(PairStore));
  for (auto& x : state_->pairs.p) {
    x.id[kPhoneIdMax] = 0;
    x.name[kPhoneNameMax] = 0;
  }
  return true;
}

size_t LanAccess::pairs_size() const { return sizeof(PairStore); }

void LanAccess::export_pairs(void* out) const { memcpy(out, &state_->pairs, sizeof(PairStore)); }

void LanAccess::set_persist(bool (*fn)(void*, const void*, size_t, const char*), void* ctx) {
  persist_ = fn;
  persist_ctx_ = ctx;
}

void LanAccess::set_random(void (*fn)(uint8_t* out, size_t n, void* ctx), void* ctx) {
  state_->random_bytes = fn;
  state_->random_ctx = ctx;
}

bool LanAccess::persist_now(const char* verifier_hex) {
  if (!persist_) return true;
  return persist_(persist_ctx_, &state_->pairs, sizeof(PairStore), verifier_hex);
}

LanKind LanAccess::authorize(const char* header, uint32_t now_ms) const {
  if (!header || strncmp(header, "Bearer ", 7) != 0) return LanKind::None;
  size_t n = strlen(header);
  if (n > 7 + 64) return LanKind::None;
  const char* token = header + 7;
  while (*token == ' ') ++token;
  if (!*token) return LanKind::None;
  if (state_->sessions.find(token, now_ms) >= 0) return LanKind::Session;
  if (state_->pairs.find(token) >= 0) return LanKind::Lan;
  return LanKind::None;
}

LoginStatus LanAccess::login(bool device_ok, const char* passcode, const char* phone, const char* name,
                             uint32_t now_ms, uint32_t unix_s, uint32_t client_key, char session[49],
                             char lan[49]) {
  size_t phone_n = phone ? strlen(phone) : 0;
  size_t name_n = name ? strlen(name) : 0;
  size_t pass_n = passcode ? strlen(passcode) : 0;
  if (!phone_n || phone_n > 40 || name_n > 32 || !pass_n || pass_n > 64) return LoginStatus::BadRequest;
  uint32_t retry = 0;
  if (state_->global.locked(now_ms, &retry) || state_->slot(client_key, now_ms).limiter.locked(now_ms, &retry)) {
    return LoginStatus::Locked;
  }
  bool ok = device_ok && has_passcode_ && checkPasscode(passcode, state_->verifier);
  if (!ok) {
    state_->global.fail(now_ms);
    state_->slot(client_key, now_ms).limiter.fail(now_ms);
    return LoginStatus::BadCredentials;
  }
  state_->slot(client_key, now_ms).limiter.success();
  if (state_->global.fails) state_->global.success();
  PairStore saved = state_->pairs;
  state_->random_token(session);
  state_->random_token(lan);
  state_->sessions.issue(session, now_ms, phone);
  state_->pairs.issue(phone, name, lan, unix_s);
  if (!persist_now(state_->verifier_hex)) {
    state_->pairs = saved;
    state_->sessions.clear();
    return LoginStatus::StorageFailed;
  }
  return LoginStatus::Ok;
}

LoginStatus LanAccess::setup_passcode(const char* passcode, const char* phone, const char* name,
                                      const uint8_t salt[16], uint32_t now_ms, uint32_t unix_s,
                                      char session[49], char lan[49]) {
  size_t phone_n = phone ? strlen(phone) : 0;
  size_t name_n = name ? strlen(name) : 0;
  if (!phone_n || phone_n > 40 || name_n > 32 || !validPasscode(passcode) || !salt) return LoginStatus::BadRequest;
  if (has_passcode_) return LoginStatus::BadCredentials;
  makeVerifier(passcode, salt, state_->verifier);
  verifierHex(state_->verifier, state_->verifier_hex);
  has_passcode_ = true;
  passcode_default_ = false;
  state_->sessions.clear();
  state_->pairs.clear();
  state_->random_token(session);
  state_->random_token(lan);
  state_->sessions.issue(session, now_ms, phone);
  state_->pairs.issue(phone, name, lan, unix_s);
  if (!persist_now(state_->verifier_hex)) {
    has_passcode_ = false;
    state_->pairs.clear();
    return LoginStatus::StorageFailed;
  }
  return LoginStatus::Ok;
}

}  // namespace powercell
