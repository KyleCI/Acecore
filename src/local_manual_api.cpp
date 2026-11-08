#include "local_manual_api.h"

#include <stdio.h>
#include <string.h>

namespace powercell {
namespace {
void reply_set(HttpReply* out, int status, const char* body) {
  out->status = status;
  snprintf(out->body, sizeof out->body, "%s", body ? body : "");
}

const char* find_key(const char* json, const char* key) {
  if (!json || !key) return nullptr;
  char pattern[64];
  snprintf(pattern, sizeof pattern, "\"%s\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return nullptr;
  p += strlen(pattern);
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
  if (*p != ':') return nullptr;
  ++p;
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
  return p;
}

bool json_string(const char* json, const char* key, char* out, size_t cap) {
  const char* p = find_key(json, key);
  if (!p || *p != '"' || cap == 0) return false;
  ++p;
  size_t n = 0;
  while (*p && *p != '"') {
    if (*p == '\\') return false;
    if (n + 1 >= cap) return false;
    out[n++] = *p++;
  }
  if (*p != '"') return false;
  out[n] = 0;
  return true;
}

bool json_int(const char* json, const char* key, int* out) {
  const char* p = find_key(json, key);
  if (!p || !out) return false;
  if (*p == '"') return false;
  int sign = 1;
  if (*p == '-') {
    sign = -1;
    ++p;
  }
  if (*p < '0' || *p > '9') return false;
  int value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    ++p;
  }
  if (*p == '.' || *p == 'e' || *p == 'E') return false;
  *out = sign * value;
  return true;
}

bool post_headers_ok(const ClientHeaders& headers, HttpReply* out) {
  if (!headers.content_type_json) {
    reply_set(out, 415, "{\"error\":\"unsupported_media\"}");
    return false;
  }
  if (!headers.accept_json) {
    reply_set(out, 406, "{\"error\":\"not_acceptable\"}");
    return false;
  }
  return true;
}

void login_error(HttpReply* out, int status, const char* error, uint32_t retry_after) {
  if (retry_after) {
    snprintf(out->body, sizeof out->body, "{\"error\":\"%s\",\"retry_after\":%u}", error, retry_after);
  } else {
    snprintf(out->body, sizeof out->body, "{\"error\":\"%s\"}", error);
  }
  out->status = status;
}

void token_body(HttpReply* out, const char* session, const char* lan, bool passcode_default) {
  snprintf(out->body, sizeof out->body,
           "{\"session\":\"%s\",\"expires_in\":1800,\"lan_token\":\"%s\",\"passcode_default\":%s}",
           session, lan, passcode_default ? "true" : "false");
  out->status = 200;
}

const char* device_id_of(const DeviceNames& device, char* buf, size_t cap) {
  if (device.stored_serial && device.stored_serial[0]) {
    snprintf(buf, cap, "%s", device.stored_serial);
    return buf;
  }
  char mac[13];
  const char* colon = device.mac_colon;
  size_t n = colon ? strlen(colon) : 0;
  if (n == 17) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < sizeof mac; ++i) {
      if (colon[i] == ':' || colon[i] == '-') continue;
      char c = colon[i];
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
      mac[o++] = c;
    }
    mac[o] = 0;
    if (o == 12) {
      snprintf(buf, cap, "ACE-%c%c%c%c%c%c", mac[6], mac[7], mac[8], mac[9], mac[10], mac[11]);
      return buf;
    }
  }
  snprintf(buf, cap, "%s", device.collector_ap ? device.collector_ap : "");
  return buf;
}
}  // namespace

bool ManualApi::persist_thunk(void* ctx, const void* pairs, size_t len, const char* verifier_hex) {
  return static_cast<ManualApi*>(ctx)->persist(ctx, pairs, len, verifier_hex);
}

bool ManualApi::persist(void*, const void*, size_t, const char* verifier_hex) {
  if (fail_persist_) return false;
  snprintf(verifier_hex_, sizeof verifier_hex_, "%s", verifier_hex ? verifier_hex : "");
  return true;
}

ManualApi::ManualApi() {
  verifier_hex_[0] = 0;
  lan_.set_persist(&ManualApi::persist_thunk, this);
}

void ManualApi::set_random(void (*fn)(uint8_t*, size_t, void*), void* ctx) { lan_.set_random(fn, ctx); }

void ManualApi::set_persist_fail(bool fail) { fail_persist_ = fail; }

void ManualApi::capture(ManualImage* out) const {
  memset(out, 0, sizeof *out);
  snprintf(out->verifier_hex, sizeof out->verifier_hex, "%s", verifier_hex_);
  out->has_passcode = lan_.has_passcode();
  out->pairs_len = lan_.pairs_size();
  if (out->pairs_len <= sizeof out->pairs) lan_.export_pairs(out->pairs);
  out->low_percent = battery_.state().low_percent;
  out->warning_percent = battery_.state().warning_percent;
  out->battery_revision = battery_.state().applied_revision;
  out->ceiling_w = ac_.state().ceiling_w;
  out->ac_mode = static_cast<int>(ac_.state().mode);
  out->ac_revision = ac_.state().applied_revision;
}

bool ManualApi::restore(const ManualImage& in) {
  lan_.clear();
  lan_.set_persist(&ManualApi::persist_thunk, this);
  verifier_hex_[0] = 0;
  if (in.has_passcode && !lan_.load_verifier_hex(in.verifier_hex)) return false;
  if (in.has_passcode) snprintf(verifier_hex_, sizeof verifier_hex_, "%s", in.verifier_hex);
  if (in.pairs_len && !lan_.load_pairs(in.pairs, in.pairs_len)) return false;
  battery_.load(in.low_percent, in.warning_percent, in.battery_revision);
  ac_.load(in.ceiling_w, in.ac_mode, in.ac_revision);
  return true;
}

LanKind ManualApi::bearer_kind(const char* authorization, uint32_t now_ms) const {
  return lan_.authorize(authorization, now_ms);
}

HttpReply ManualApi::setup_info(const DeviceNames& device, const UnitFacts& facts) {
  HttpReply out;
  char id[64];
  device_id_of(device, id, sizeof id);
  snprintf(out.body, sizeof out.body,
           "{\"device_id\":\"%s\",\"model\":\"%s\",\"fw\":\"%s\",\"state\":\"%s\","
           "\"claimed\":false,\"passcode_default\":%s,\"passcode_set\":%s,\"hotspot\":%s,"
           "\"onboarding_version\":2,\"powercell_model\":null,\"rated_kva\":null,\"usable_kw\":null}",
           id, facts.model ? facts.model : "esp32dev", facts.firmware ? facts.firmware : "1.0.1",
           facts.wifi_configured ? "configured" : "unconfigured",
           lan_.passcode_default() ? "true" : "false", lan_.has_passcode() ? "true" : "false",
           facts.hotspot ? "true" : "false");
  out.status = 200;
  return out;
}

HttpReply ManualApi::login(const char* json, const ClientHeaders& headers, const DeviceNames& device,
                           uint32_t now_ms, uint32_t unix_s, uint32_t client_key) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  char id[64], pass[68], phone[48], name[40];
  if (!json_string(json, "device_id", id, sizeof id) || !json_string(json, "passcode", pass, sizeof pass) ||
      !json_string(json, "phone_id", phone, sizeof phone) ||
      !json_string(json, "phone_name", name, sizeof name)) {
    login_error(&out, 400, "invalid_request", 0);
    return out;
  }
  char session[49], lan[49];
  uint32_t retry = 0;
  const LoginStatus status = lan_.login(login_device_matches(DeviceNames{id, device.mac_colon, device.collector_ap,
                                                                          device.stored_gw_name, device.stored_serial}),
                                        pass, phone, name, now_ms, unix_s, client_key, session, lan, &retry);
  if (status == LoginStatus::Ok) {
    token_body(&out, session, lan, lan_.passcode_default());
    return out;
  }
  if (status == LoginStatus::BadRequest) login_error(&out, 400, "invalid_request", 0);
  else if (status == LoginStatus::Locked) login_error(&out, 429, "locked", retry);
  else if (status == LoginStatus::StorageFailed) login_error(&out, 500, "storage_failed", 0);
  else login_error(&out, 401, "bad_credentials", retry);
  return out;
}

HttpReply ManualApi::setup_passcode(const char* json, const ClientHeaders& headers, bool on_hotspot,
                                    const uint8_t salt[16], uint32_t now_ms, uint32_t unix_s) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (lan_.has_passcode()) {
    login_error(&out, 409, "passcode_already_set", 0);
    return out;
  }
  if (!on_hotspot) {
    login_error(&out, 403, "hotspot_required", 0);
    return out;
  }
  char pass[68], phone[48], name[40];
  if (!json_string(json, "passcode", pass, sizeof pass) || !json_string(json, "phone_id", phone, sizeof phone) ||
      !json_string(json, "phone_name", name, sizeof name)) {
    login_error(&out, 400, "invalid_request", 0);
    return out;
  }
  char session[49], lan[49];
  const LoginStatus status = lan_.setup_passcode(pass, phone, name, salt, now_ms, unix_s, session, lan);
  if (status == LoginStatus::Ok) {
    token_body(&out, session, lan, false);
    return out;
  }
  if (status == LoginStatus::StorageFailed) login_error(&out, 500, "storage_failed", 0);
  else if (status == LoginStatus::BadCredentials) login_error(&out, 409, "passcode_already_set", 0);
  else login_error(&out, 422, "invalid_passcode", 0);
  return out;
}

HttpReply ManualApi::status(const char* authorization, const DeviceNames& device, const UnitFacts& facts,
                            uint32_t now_ms) {
  HttpReply out;
  const LanKind kind = bearer_kind(authorization, now_ms);
  if (kind != LanKind::Session && kind != LanKind::Lan) {
    login_error(&out, 401, "unauthorized", 0);
    return out;
  }
  char id[64];
  device_id_of(device, id, sizeof id);
  snprintf(out.body, sizeof out.body,
           "{\"device_id\":\"%s\",\"fw\":\"%s\",\"hotspot\":%s,\"sta_connected\":%s,"
           "\"payment_locked\":%s,\"payment_lock_reason\":\"\",\"cloud_linked\":false,"
           "\"auth\":\"%s\"}",
           id, facts.firmware ? facts.firmware : "1.0.1", facts.hotspot ? "true" : "false",
           facts.sta_connected ? "true" : "false", facts.payment_locked ? "true" : "false",
           kind == LanKind::Session ? "session" : "lan");
  out.status = 200;
  return out;
}

int ManualApi::data_auth(const char* authorization, uint32_t now_ms) const {
  if (!authorization || !authorization[0]) return 200;
  return bearer_kind(authorization, now_ms) == LanKind::Lan ? 200 : 401;
}

HttpReply ManualApi::battery_get(const char* authorization, uint32_t now_ms) {
  HttpReply out;
  if (bearer_kind(authorization, now_ms) != LanKind::Lan) {
    reply_set(&out, 401, "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"unauthorized\",\"applied\":false,"
                         "\"durable_commit\":false,\"pending\":false,\"readback_present\":false}");
    return out;
  }
  const SettingsState& s = battery_.state();
  snprintf(out.body, sizeof out.body,
           "{\"schema\":1,\"result\":\"readback\",\"reason\":\"none\",\"applied\":false,"
           "\"durable_commit\":%s,\"pending\":false,\"physical_shutdown_verified\":false,"
           "\"readback\":{\"schema\":1,\"low_percent\":%d,\"warning_percent\":%d,"
           "\"inhibit_percent\":10,\"restart_percent\":15,\"applied_revision\":%u,\"durable\":true}}",
           s.durable ? "true" : "false", s.low_percent, s.warning_percent, s.applied_revision);
  out.status = 200;
  return out;
}

HttpReply ManualApi::battery_post(const char* authorization, const char* json, const ClientHeaders& headers,
                                  uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (bearer_kind(authorization, now_ms) != LanKind::Lan) {
    reply_set(&out, 401, "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"unauthorized\",\"applied\":false,"
                         "\"durable_commit\":false,\"pending\":false,\"readback_present\":false}");
    return out;
  }
  int schema = 0, low = 0, warning = 0, revision = 0;
  const bool schema_int = json_int(json, "schema", &schema);
  const bool low_int = json_int(json, "low_percent", &low);
  const bool warning_int = json_int(json, "warning_percent", &warning);
  const bool revision_int = json_int(json, "base_revision", &revision);
  if (!schema_int || !revision_int) {
    reply_set(&out, 400, "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"not_integer\",\"applied\":false,"
                         "\"durable_commit\":false,\"pending\":false}");
    return out;
  }
  const bool persist_ok = !fail_persist_;
  const SettingsDecision decision =
      battery_.set(SettingsAuth::Ok, schema, low_int, low, warning_int, warning,
                   static_cast<uint32_t>(revision), persist_ok);
  const int code = decision.applied ? 200 : (decision.reason == SettingsReason::PersistFailed ? 500 : 400);
  snprintf(out.body, sizeof out.body,
           "{\"schema\":1,\"result\":\"%s\",\"reason\":\"%s\",\"applied\":%s,\"durable_commit\":%s,"
           "\"pending\":false,\"physical_shutdown_verified\":false,"
           "\"readback\":{\"schema\":1,\"low_percent\":%d,\"warning_percent\":%d,\"inhibit_percent\":10,"
           "\"restart_percent\":15,\"applied_revision\":%u,\"durable\":%s}}",
           decision.applied ? "applied" : "rejected", settings_reason_name(decision.reason),
           decision.applied ? "true" : "false", decision.durable_commit ? "true" : "false",
           decision.readback.low_percent, decision.readback.warning_percent, decision.readback.applied_revision,
           decision.readback.durable ? "true" : "false");
  out.status = code;
  return out;
}

HttpReply ManualApi::ac_get(const char* authorization, uint32_t now_ms) {
  HttpReply out;
  if (bearer_kind(authorization, now_ms) != LanKind::Lan) {
    reply_set(&out, 401, "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"unauthorized\",\"applied\":false,"
                         "\"readback_present\":false,\"shutdown_operational\":false,"
                         "\"total_output_coverage_verified\":false}");
    return out;
  }
  const AcState& s = ac_.state();
  snprintf(out.body, sizeof out.body,
           "{\"schema\":1,\"result\":\"readback\",\"applied\":false,\"pending\":false,"
           "\"shutdown_operational\":false,\"physical_shutdown_verified\":false,"
           "\"total_output_coverage_verified\":false,\"selected_source\":\"pzem004t_v3_active_power\","
           "\"readback\":{\"ceiling_w\":%d,\"mode\":\"%s\",\"applied_revision\":%u}}",
           s.ceiling_w, s.mode == AcModeSetting::WarnAndShutdown ? "warn_and_shutdown" : "warn_only",
           s.applied_revision);
  out.status = 200;
  return out;
}

HttpReply ManualApi::ac_post(const char* authorization, const char* json, const ClientHeaders& headers,
                             uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (bearer_kind(authorization, now_ms) != LanKind::Lan) {
    reply_set(&out, 401, "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"unauthorized\",\"applied\":false,"
                         "\"shutdown_operational\":false,\"readback_present\":false}");
    return out;
  }
  int schema = 0, ceiling = 0, revision = 0;
  char mode_text[32];
  const bool schema_int = json_int(json, "schema", &schema);
  const bool ceiling_int = json_int(json, "ceiling_w", &ceiling);
  const bool revision_int = json_int(json, "base_revision", &revision);
  const bool mode_ok = json_string(json, "mode", mode_text, sizeof mode_text);
  int mode = -1;
  if (mode_ok && strcmp(mode_text, "warn_only") == 0) mode = 0;
  if (mode_ok && strcmp(mode_text, "warn_and_shutdown") == 0) mode = 1;
  if (!schema_int || !revision_int) {
    reply_set(&out, 400, "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"not_integer\",\"applied\":false,"
                         "\"shutdown_operational\":false}");
    return out;
  }
  const AcDecision decision = ac_.set(SettingsAuth::Ok, schema, ceiling_int, ceiling, mode >= 0, mode,
                                      static_cast<uint32_t>(revision), !fail_persist_);
  snprintf(out.body, sizeof out.body,
           "{\"schema\":1,\"result\":\"%s\",\"reason\":\"%s\",\"applied\":%s,\"durable_commit\":%s,"
           "\"pending\":false,\"shutdown_operational\":false,\"physical_shutdown_verified\":false,"
           "\"total_output_coverage_verified\":false,"
           "\"readback\":{\"ceiling_w\":%d,\"mode\":\"%s\",\"applied_revision\":%u}}",
           decision.applied ? "applied" : "rejected", settings_reason_name(decision.reason),
           decision.applied ? "true" : "false", decision.durable_commit ? "true" : "false",
           decision.readback.ceiling_w,
           decision.readback.mode == AcModeSetting::WarnAndShutdown ? "warn_and_shutdown" : "warn_only",
           decision.readback.applied_revision);
  out.status = decision.applied ? 200 : (decision.reason == SettingsReason::PersistFailed ? 500 : 400);
  return out;
}

}  // namespace powercell
