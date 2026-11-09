#include "local_manual_api.h"
#include "display_identity.h"

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
  if (factory_serial_valid(device.stored_serial)) {
    snprintf(buf, cap, "%s", device.stored_serial);
    return buf;
  }
  if (cap) buf[0] = 0;
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

ManualApi::ManualApi(int factory_rated_w, const char* factory_variant)
    : rating_(factory_rated_w, factory_variant) {
  verifier_hex_[0] = 0;
  claim_code_[0] = 0;
  ap_pass_[0] = 0;
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
  out->rating_store_present = rating_.state().store_present;
  out->stored_rated_active_w = rating_.state().stored_rated_w;
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
  rating_.observeStore(in.rating_store_present, in.stored_rated_active_w);
  return true;
}

LanKind ManualApi::bearer_kind(const char* authorization, uint32_t now_ms) const {
  return lan_.authorize(authorization, now_ms);
}

HttpReply ManualApi::setup_info(const DeviceNames& device, const UnitFacts& facts) {
  HttpReply out;
  char id[64];
  device_id_of(device, id, sizeof id);
  char id_json[80];
  if (id[0]) snprintf(id_json, sizeof id_json, "\"%s\"", id);
  else snprintf(id_json, sizeof id_json, "null");
  const int usable = rating_.usableW();
  const char* powercell = "null";
  const char* usable_kw = "null";
  if (usable == 4500) {
    powercell = "\"PC5K\"";
    usable_kw = "4.5";
  } else if (usable == 3150) {
    usable_kw = "3.15";
  }
  snprintf(out.body, sizeof out.body,
           "{\"device_id\":%s,\"model\":\"esp32dev\",\"fw\":\"%s\",\"state\":\"%s\","
           "\"claimed\":%s,\"passcode_default\":%s,\"passcode_set\":%s,\"hotspot\":%s,"
           "\"onboarding_version\":2,\"powercell_model\":%s,\"rated_kva\":null,\"usable_kw\":%s}",
           id_json, facts.firmware ? facts.firmware : "1.0.1",
           facts.wifi_configured ? "configured" : "unconfigured", facts.claimed ? "true" : "false",
           lan_.passcode_default() ? "true" : "false", lan_.has_passcode() ? "true" : "false",
           facts.hotspot ? "true" : "false", powercell, usable_kw);
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
  char id[64], id_json[80];
  device_id_of(device, id, sizeof id);
  if (id[0]) snprintf(id_json, sizeof id_json, "\"%s\"", id);
  else snprintf(id_json, sizeof id_json, "null");
  const RatingState& rating = rating_.state();
  if (rating.overload_configured) {
    snprintf(out.body, sizeof out.body,
             "{\"device_id\":%s,\"fw\":\"%s\",\"hotspot\":%s,\"sta_connected\":%s,"
             "\"payment_locked\":%s,\"payment_lock_reason\":\"\",\"cloud_linked\":%s,"
             "\"auth\":\"%s\",\"factory_variant\":\"%s\",\"rated_active_w\":%d,"
             "\"usable_active_w\":%d,\"overload_configured\":true,\"rating_writable\":false,"
             "\"rating_reason\":\"none\",\"total_output_coverage_verified\":false}",
             id_json, facts.firmware ? facts.firmware : "1.0.1", facts.hotspot ? "true" : "false",
             facts.sta_connected ? "true" : "false", facts.payment_locked ? "true" : "false",
             facts.claimed ? "true" : "false", kind == LanKind::Session ? "session" : "lan",
             rating.variant, rating.factory_rated_w, rating.usable_active_w);
  } else {
    snprintf(out.body, sizeof out.body,
             "{\"device_id\":%s,\"fw\":\"%s\",\"hotspot\":%s,\"sta_connected\":%s,"
             "\"payment_locked\":%s,\"payment_lock_reason\":\"\",\"cloud_linked\":%s,"
             "\"auth\":\"%s\",\"factory_variant\":\"%s\",\"rated_active_w\":null,"
             "\"usable_active_w\":null,\"overload_configured\":false,\"rating_writable\":false,"
             "\"rating_reason\":\"%s\",\"total_output_coverage_verified\":false}",
             id_json, facts.firmware ? facts.firmware : "1.0.1", facts.hotspot ? "true" : "false",
             facts.sta_connected ? "true" : "false", facts.payment_locked ? "true" : "false",
             facts.claimed ? "true" : "false", kind == LanKind::Session ? "session" : "lan",
             rating.variant, rating_.reason());
  }
  out.status = 200;
  return out;
}

int ManualApi::data_auth(const char* authorization, uint32_t now_ms) const {
  if (!authorization || !authorization[0]) return 200;
  return bearer_kind(authorization, now_ms) == LanKind::Lan ? 200 : 401;
}

HttpReply ManualApi::battery_get(const char* authorization, uint32_t now_ms) {
  HttpReply out;
  const SettingsAuth auth = classify_lan_authorization(authorization, bearer_kind(authorization, now_ms));
  if (auth != SettingsAuth::Ok) {
    const char* reason = settings_reason_name(auth == SettingsAuth::SessionOnly
                                                 ? SettingsReason::SessionNotSufficient
                                                 : SettingsReason::Unauthorized);
    snprintf(out.body, sizeof out.body,
             "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"%s\",\"applied\":false,"
             "\"durable_commit\":false,\"pending\":false,\"readback_present\":false}",
             reason);
    out.status = 401;
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
  const SettingsAuth auth = classify_lan_authorization(authorization, bearer_kind(authorization, now_ms));
  if (auth != SettingsAuth::Ok) {
    const char* reason = settings_reason_name(auth == SettingsAuth::SessionOnly
                                                 ? SettingsReason::SessionNotSufficient
                                                 : SettingsReason::Unauthorized);
    snprintf(out.body, sizeof out.body,
             "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"%s\",\"applied\":false,"
             "\"durable_commit\":false,\"pending\":false,\"readback_present\":false}",
             reason);
    out.status = 401;
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
      battery_.set(auth, schema, low_int, low, warning_int, warning,
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
  const SettingsAuth ac_get_auth = classify_lan_authorization(authorization, bearer_kind(authorization, now_ms));
  if (ac_get_auth != SettingsAuth::Ok) {
    const char* reason = settings_reason_name(ac_get_auth == SettingsAuth::SessionOnly
                                                  ? SettingsReason::SessionNotSufficient
                                                  : SettingsReason::Unauthorized);
    snprintf(out.body, sizeof out.body,
             "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"%s\",\"applied\":false,"
             "\"readback_present\":false,\"shutdown_operational\":false,"
             "\"total_output_coverage_verified\":false}",
             reason);
    out.status = 401;
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
  const SettingsAuth ac_post_auth = classify_lan_authorization(authorization, bearer_kind(authorization, now_ms));
  if (ac_post_auth != SettingsAuth::Ok) {
    const char* reason = settings_reason_name(ac_post_auth == SettingsAuth::SessionOnly
                                                  ? SettingsReason::SessionNotSufficient
                                                  : SettingsReason::Unauthorized);
    snprintf(out.body, sizeof out.body,
             "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"%s\",\"applied\":false,"
             "\"durable_commit\":false,\"pending\":false,\"shutdown_operational\":false,"
             "\"readback_present\":false}",
             reason);
    out.status = 401;
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
                                      rating_.state().usable_active_w, static_cast<uint32_t>(revision),
                                      !fail_persist_);
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

void format_rating(char* buf, size_t cap, const char* result, const char* reason, bool applied,
                   bool durable_commit, const RatingState& s) {
  char rated[16], usable[16], factory[16], stored[16];
  snprintf(rated, sizeof rated, "null");
  snprintf(usable, sizeof usable, "null");
  snprintf(factory, sizeof factory, "null");
  snprintf(stored, sizeof stored, "null");
  if (s.factory_rated_w != 0) snprintf(factory, sizeof factory, "%d", s.factory_rated_w);
  if (s.store_present) snprintf(stored, sizeof stored, "%d", s.stored_rated_w);
  if (s.overload_configured) {
    snprintf(rated, sizeof rated, "%d", s.factory_rated_w);
    snprintf(usable, sizeof usable, "%d", s.usable_active_w);
  }
  snprintf(buf, cap,
           "{\"schema\":1,\"result\":\"%s\",\"reason\":\"%s\",\"applied\":%s,\"durable_commit\":%s,"
           "\"writable\":false,\"authority\":\"factory_image\",\"overload_configured\":%s,"
           "\"physical_shutdown_verified\":false,\"factory_variant\":\"%s\","
           "\"readback\":{\"rated_active_w\":%s,\"usable_active_w\":%s,\"factory_rated_active_w\":%s,"
           "\"stored_rated_active_w\":%s,\"rating_match\":%s}}",
           result, reason, applied ? "true" : "false", durable_commit ? "true" : "false",
           s.overload_configured ? "true" : "false", s.variant ? s.variant : "unrated", rated, usable,
           factory, stored, s.overload_configured ? "true" : "false");
}

HttpReply ManualApi::rating_get(const char* authorization, uint32_t now_ms) {
  HttpReply out;
  const SettingsAuth auth = classify_lan_authorization(authorization, bearer_kind(authorization, now_ms));
  if (auth != SettingsAuth::Ok) {
    const char* reason = settings_reason_name(auth == SettingsAuth::SessionOnly
                                                  ? SettingsReason::SessionNotSufficient
                                                  : SettingsReason::Unauthorized);
    snprintf(out.body, sizeof out.body,
             "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"%s\",\"applied\":false,"
             "\"readback_present\":false}",
             reason);
    out.status = 401;
    return out;
  }
  format_rating(out.body, sizeof out.body, "readback", rating_.reason(), false, false, rating_.state());
  out.status = 200;
  return out;
}

HttpReply ManualApi::rating_post(const char* authorization, const char* json, const ClientHeaders& headers,
                                 uint32_t now_ms) {
  (void)json;
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  const SettingsAuth auth = classify_lan_authorization(authorization, bearer_kind(authorization, now_ms));
  if (auth != SettingsAuth::Ok) {
    const char* reason = settings_reason_name(auth == SettingsAuth::SessionOnly
                                                  ? SettingsReason::SessionNotSufficient
                                                  : SettingsReason::Unauthorized);
    snprintf(out.body, sizeof out.body,
             "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"%s\",\"applied\":false,"
             "\"durable_commit\":false,\"readback_present\":false}",
             reason);
    out.status = 401;
    return out;
  }
  const RatingState before = rating_.state();
  format_rating(out.body, sizeof out.body, "rejected",
                settings_reason_name(SettingsReason::FactoryProvisioned), false, false, before);
  out.status = 403;
  return out;
}

bool legacy_power_allowed(const char* authorization, uint32_t now_ms, const LanAccess& lan) {
  const LanKind kind = lan.authorize(authorization, now_ms);
  const SettingsAuth auth = classify_lan_authorization(authorization, kind);
  return auth == SettingsAuth::Ok || auth == SettingsAuth::SessionOnly;
}

RouteEffect ManualApi::gateway_state(const char* authorization, const char* json, const ClientHeaders& headers,
                                     OutputProfile profile, bool output_on_allowed, bool ota_busy,
                                     bool payment_locked, uint32_t now_ms) {
  RouteEffect effect;
  if (!post_headers_ok(headers, &effect.http)) return effect;
  if (!legacy_power_allowed(authorization, now_ms, lan_)) {
    reply_set(&effect.http, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return effect;
  }
  char state[8];
  if (!json_string(json, "state", state, sizeof state) ||
      (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
    reply_set(&effect.http, 400, "{\"ok\":false,\"error\":\"state must be on|off\"}");
    return effect;
  }
  const bool on = strcmp(state, "on") == 0;
  if (payment_locked) {
    reply_set(&effect.http, 403, "{\"ok\":false,\"error\":\"payment_locked\"}");
    return effect;
  }
  if (on && ota_busy) {
    reply_set(&effect.http, 409, "{\"ok\":false,\"error\":\"refused\"}");
    return effect;
  }
  if (on && !output_on_allowed) {
    reply_set(&effect.http, 409, "{\"ok\":false,\"error\":\"battery_output_inhibited\"}");
    return effect;
  }
  const bool v011 = profile == OutputProfile::V011Dsp;
  const int level = v011 ? v011_dsp_level(on) : original_relay_level(on);
  snprintf(effect.http.body, sizeof effect.http.body,
           "{\"ok\":true,\"state\":\"%s\",\"applied_via\":\"%s\",\"measured_shutdown\":false,"
           "\"aceplug\":false,\"gpio_level\":%d}",
           state, v011 ? "dsp_gpio15" : "collector_relay_gpio15", level);
  effect.http.status = 200;
  effect.drive_gateway = true;
  effect.gateway_on = on;
  return effect;
}

RouteEffect ManualApi::socket_relay(const char* authorization, const char* json, const ClientHeaders& headers,
                                    bool lora_fitted, bool socket_registered, bool output_on_allowed,
                                    bool payment_locked, uint32_t now_ms) {
  RouteEffect effect;
  if (!post_headers_ok(headers, &effect.http)) return effect;
  if (!legacy_power_allowed(authorization, now_ms, lan_)) {
    reply_set(&effect.http, 401, "{\"ok\":false,\"error\":\"unauthorized\",\"aceplug\":true,\"dsp_gpio15\":false}");
    return effect;
  }
  char id[40], state[8];
  if (!json_string(json, "socket_id", id, sizeof id) && !json_string(json, "id", id, sizeof id)) id[0] = 0;
  if (!json_string(json, "state", state, sizeof state) || !id[0] ||
      (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
    reply_set(&effect.http, 400,
              "{\"ok\":false,\"error\":\"socket_id and state=on|off required\",\"aceplug\":true,\"dsp_gpio15\":false}");
    return effect;
  }
  if (!lora_fitted) {
    reply_set(&effect.http, 409, "{\"ok\":false,\"error\":\"lora_not_fitted\",\"aceplug\":true,\"dsp_gpio15\":false}");
    return effect;
  }
  if (payment_locked) {
    reply_set(&effect.http, 403, "{\"ok\":false,\"error\":\"payment_locked\",\"aceplug\":true,\"dsp_gpio15\":false}");
    return effect;
  }
  const bool on = strcmp(state, "on") == 0;
  if (on && !output_on_allowed) {
    reply_set(&effect.http, 409,
              "{\"ok\":false,\"error\":\"battery_output_inhibited\",\"aceplug\":true,\"dsp_gpio15\":false}");
    return effect;
  }
  if (!socket_registered) {
    reply_set(&effect.http, 404, "{\"ok\":false,\"error\":\"unknown socket\",\"aceplug\":true,\"dsp_gpio15\":false}");
    return effect;
  }
  snprintf(effect.http.body, sizeof effect.http.body,
           "{\"ok\":true,\"message\":\"queued\",\"state\":\"%s\",\"socket_id\":\"%s\",\"aceplug\":true,\"dsp_gpio15\":false}",
           state, id);
  effect.http.status = 202;
  effect.queue_aceplug = true;
  effect.aceplug_on = on;
  snprintf(effect.socket_id, sizeof effect.socket_id, "%s", id);
  return effect;
}

namespace {
bool bearer_ok(LanKind kind, bool session_only, HttpReply* out) {
  if (session_only) {
    if (kind == LanKind::Session) return true;
    login_error(out, kind == LanKind::Lan ? 403 : 401, kind == LanKind::Lan ? "session_required" : "unauthorized", 0);
    return false;
  }
  if (kind == LanKind::Session || kind == LanKind::Lan) return true;
  login_error(out, 401, "unauthorized", 0);
  return false;
}
}  // namespace

HttpReply ManualApi::wifi_scan(const char* authorization, uint32_t now_ms) {
  HttpReply out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  reply_set(&out, 200, "{\"scanning\":true,\"networks\":[]}");
  return out;
}

HttpReply ManualApi::wifi_test(const char* authorization, const char* json, const ClientHeaders& headers,
                               uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  char ssid[33], pass[64];
  if (!json_string(json, "ssid", ssid, sizeof ssid)) {
    login_error(&out, 400, "invalid_request", 0);
    return out;
  }
  if (!json_string(json, "password", pass, sizeof pass)) pass[0] = 0;
  if (!wifi_.start(ssid, pass, "ab12cd34")) {
    login_error(&out, 409, "test_busy_or_invalid", 0);
    return out;
  }
  snprintf(out.body, sizeof out.body, "{\"test_id\":\"%s\"}", wifi_.test_id);
  out.status = 202;
  return out;
}

HttpReply ManualApi::wifi_status(const char* authorization, uint32_t now_ms) {
  HttpReply out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  const char* state = wifi_.state == 2 ? "ok" : wifi_.state == 3 ? "failed" : wifi_.state == 1 ? "testing" : "idle";
  if (wifi_.state == 2) {
    snprintf(out.body, sizeof out.body, "{\"state\":\"ok\",\"test_id\":\"%s\",\"ip\":\"%s\"}", wifi_.test_id, wifi_.ip);
  } else if (wifi_.test_id[0]) {
    snprintf(out.body, sizeof out.body, "{\"state\":\"%s\",\"test_id\":\"%s\"}", state, wifi_.test_id);
  } else {
    snprintf(out.body, sizeof out.body, "{\"state\":\"idle\"}");
  }
  out.status = 200;
  return out;
}

HttpReply ManualApi::wifi_save(const char* authorization, const ClientHeaders& headers, uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  char ip[16];
  if (!wifi_.save(ip, sizeof ip)) {
    login_error(&out, 409, "no_successful_test", 0);
    return out;
  }
  snprintf(out.body, sizeof out.body, "{\"ok\":true,\"ip\":\"%s\",\"hotspot_off_in\":120}", ip);
  out.status = 200;
  return out;
}

void ManualApi::wifi_test_succeeded(const char* ip) { wifi_.succeed(ip); }

HttpReply ManualApi::network_mode(const char* authorization, const char* json, const ClientHeaders& headers,
                                  uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), true, &out)) return out;
  char mode[16];
  if (!json_string(json, "mode", mode, sizeof mode) ||
      (strcmp(mode, "station") != 0 && strcmp(mode, "hotspot_only") != 0)) {
    login_error(&out, 400, "invalid_mode", 0);
    return out;
  }
  snprintf(wifi_.mode, sizeof wifi_.mode, "%s", mode);
  snprintf(out.body, sizeof out.body, "{\"ok\":true,\"mode\":\"%s\"}", mode);
  out.status = 200;
  return out;
}

HttpReply ManualApi::ap_password(const char* authorization, const char* json, const ClientHeaders& headers,
                                 uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  char next[64];
  if (!json_string(json, "new", next, sizeof next) || strlen(next) < 8 || strlen(next) > 63) {
    login_error(&out, 400, "invalid_password", 0);
    return out;
  }
  snprintf(ap_pass_, sizeof ap_pass_, "%s", next);
  reply_set(&out, 200, "{\"ok\":true,\"applies\":\"after_reconnect\"}");
  return out;
}

HttpReply ManualApi::claim_code(const char* authorization, const ClientHeaders& headers, uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  snprintf(claim_code_, sizeof claim_code_, "ABCDEFGH");
  snprintf(out.body, sizeof out.body, "{\"claim_code\":\"%s\",\"expires_in\":900}", claim_code_);
  out.status = 200;
  return out;
}

void ManualApi::observe_clock(int64_t now_s, int source, bool ntp_fresh) {
  clock_now_ = now_s;
  clock_src_ = source;
  ntp_fresh_ = ntp_fresh;
}

HttpReply ManualApi::set_time(const char* authorization, const char* json, const ClientHeaders& headers,
                              uint32_t now_ms) {
  HttpReply out;
  if (!post_headers_ok(headers, &out)) return out;
  if (!bearer_ok(bearer_kind(authorization, now_ms), false, &out)) return out;
  int epoch = 0;
  if (!json_int(json, "epoch", &epoch)) {
    login_error(&out, 400, "invalid_request", 0);
    return out;
  }
  int tz = 0;
  bool has_tz = json_int(json, "tz_offset_min", &tz);
  if (has_tz && !pc::clk::validTz(tz)) {
    login_error(&out, 400, "invalid_tz_offset", 0);
    return out;
  }
  pc::clk::Source src = clock_src_ == 3 ? pc::clk::Source::Ntp
                        : clock_src_ == 2 ? pc::clk::Source::Phone
                        : clock_src_ == 1 ? pc::clk::Source::Nvs
                                          : pc::clk::Source::None;
  pc::clk::Verdict verdict = pc::clk::judge(epoch, clock_now_, src, ntp_fresh_);
  if (verdict == pc::clk::Verdict::Invalid) {
    login_error(&out, 400, "invalid_epoch", 0);
    return out;
  }
  if (verdict == pc::clk::Verdict::Accept) {
    clock_now_ = epoch;
    clock_src_ = 2;
    reply_set(&out, 200, "{\"ok\":true,\"applied\":true}");
    return out;
  }
  snprintf(out.body, sizeof out.body, "{\"ok\":true,\"applied\":false,\"reason\":\"%s\"}", pc::clk::verdictName(verdict));
  out.status = 200;
  return out;
}

}  // namespace powercell
