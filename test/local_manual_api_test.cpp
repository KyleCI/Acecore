#include "home_snapshot.h"
#include "local_manual_api.h"
#include "output_gate.h"
#include "power_units.h"

#include <stdio.h>
#include <string.h>

using namespace powercell;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static uint32_t g_counter = 1;
static void fill_random(uint8_t* out, size_t n, void*) {
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(g_counter++);
}

static const char* field(const char* json, const char* key, char* out, size_t cap) {
  char pattern[64];
  snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return nullptr;
  p += strlen(pattern);
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return out;
}

static ClientHeaders json_headers() {
  ClientHeaders headers;
  headers.content_type_json = true;
  headers.accept_json = true;
  return headers;
}

static void login_unit(ManualApi* api, const DeviceNames& device, char* bearer_lan, size_t cap) {
  api->set_random(fill_random, nullptr);
  uint8_t salt[16];
  memset(salt, 9, sizeof salt);
  const char* setup =
      "{\"passcode\":\"135790\",\"phone_id\":\"11111111-2222-3333-4444-555555555555\","
      "\"phone_name\":\"Powercore\"}";
  CHECK(api->setup_passcode(setup, json_headers(), true, salt, 1000, 0).status == 200);
  const char* login_json =
      "{\"device_id\":\"ACE-000123\",\"passcode\":\"135790\","
      "\"phone_id\":\"11111111-2222-3333-4444-555555555555\",\"phone_name\":\"Powercore\"}";
  HttpReply logged = api->login(login_json, json_headers(), device, 5000, 1700001000, 9);
  char lan[80];
  CHECK(field(logged.body, "lan_token", lan, sizeof lan) && lan[0]);
  snprintf(bearer_lan, cap, "Bearer %s", lan);
}

static void other_factory_images(const DeviceNames& device, const UnitFacts& facts) {
  char bearer[96];
  ManualApi small(3500, "hw-v0.1.1-3.5kw");
  login_unit(&small, device, bearer, sizeof bearer);
  const char* small_get =
      "{\"schema\":1,\"result\":\"readback\",\"reason\":\"none\",\"applied\":false,\"durable_commit\":false,"
      "\"writable\":false,\"authority\":\"factory_image\",\"overload_configured\":true,"
      "\"physical_shutdown_verified\":false,\"factory_variant\":\"hw-v0.1.1-3.5kw\","
      "\"readback\":{\"rated_active_w\":3500,\"usable_active_w\":3150,\"factory_rated_active_w\":3500,"
      "\"stored_rated_active_w\":null,\"rating_match\":true}}";
  CHECK(strcmp(small.rating_get(bearer, 1000).body, small_get) == 0);
  CHECK(strstr(small.status(bearer, device, facts, 1000).body, "\"usable_active_w\":3150"));
  HttpReply at_bound = small.ac_post(
      bearer, "{\"schema\":1,\"ceiling_w\":3150,\"mode\":\"warn_only\",\"base_revision\":0}", json_headers(), 1000);
  CHECK(at_bound.status == 200 && strstr(at_bound.body, "\"ceiling_w\":3150"));
  HttpReply over = small.ac_post(
      bearer, "{\"schema\":1,\"ceiling_w\":3151,\"mode\":\"warn_only\",\"base_revision\":1}", json_headers(), 1000);
  CHECK(over.status == 400 && strstr(over.body, "\"reason\":\"ceiling_above_usable\""));
  CHECK(strstr(small.ac_get(bearer, 1000).body, "\"ceiling_w\":3150"));

  ManualApi missing;
  login_unit(&missing, device, bearer, sizeof bearer);
  HttpReply missing_get = missing.rating_get(bearer, 1000);
  CHECK(missing_get.status == 200);
  CHECK(strstr(missing_get.body, "\"reason\":\"rating_unprovisioned\""));
  CHECK(strstr(missing_get.body, "\"overload_configured\":false"));
  CHECK(strstr(missing_get.body, "\"rated_active_w\":null"));
  CHECK(strstr(missing_get.body, "\"usable_active_w\":null"));
  CHECK(strstr(missing_get.body, "\"factory_variant\":\"unrated\""));
  CHECK(strstr(missing_get.body, "4500") == nullptr);
  CHECK(strstr(missing_get.body, "3150") == nullptr);
  CHECK(missing.rating_post(bearer, "{\"schema\":1,\"rated_active_w\":5000,\"base_revision\":0}", json_headers(),
                            1000)
            .status == 403);
  CHECK(!missing.rating().armed());
  CHECK(missing.rating().usableW() == 0);
  CHECK(missing.ac_post(bearer, "{\"schema\":1,\"ceiling_w\":500,\"mode\":\"warn_only\",\"base_revision\":0}",
                        json_headers(), 1000)
            .status == 400);
  CHECK(strstr(missing.status(bearer, device, facts, 1000).body, "\"overload_configured\":false"));

  ManualApi unknown(4000, "hw-v0.1.1-bad");
  login_unit(&unknown, device, bearer, sizeof bearer);
  HttpReply unknown_get = unknown.rating_get(bearer, 1000);
  CHECK(strstr(unknown_get.body, "\"reason\":\"unknown_rating\""));
  CHECK(strstr(unknown_get.body, "\"factory_rated_active_w\":4000"));
  CHECK(strstr(unknown_get.body, "\"usable_active_w\":null"));
  CHECK(strstr(unknown_get.body, "3600") == nullptr);
  CHECK(!unknown.rating().armed());
}

int main() {
  DeviceNames device;
  device.requested = nullptr;
  device.mac_colon = "AA:BB:CC:DD:EE:FF";
  device.collector_ap = "Acecore PowerCell LabTest";
  device.stored_gw_name = nullptr;
  device.stored_serial = "ACE-000123";
  UnitFacts facts;
  facts.wifi_configured = false;
  facts.model = "hw-v0.1.1";
  facts.firmware = "1.0.1";

  ManualApi api(5000, "hw-v0.1.1-5kw");
  api.set_random(fill_random, nullptr);
  HttpReply info = api.setup_info(device, facts);
  CHECK(info.status == 200);
  CHECK(strstr(info.body, "\"device_id\":\"ACE-000123\""));
  CHECK(strstr(info.body, "\"model\":\"esp32dev\""));
  CHECK(strstr(info.body, "\"state\":\"unconfigured\""));
  CHECK(strstr(info.body, "\"passcode_set\":false"));
  CHECK(strstr(info.body, "\"powercell_model\":\"PC5K\""));
  CHECK(strstr(info.body, "\"rated_kva\":null"));
  CHECK(strstr(info.body, "\"usable_kw\":4.5"));
  CHECK(strstr(info.body, "\"units\":{\"power\":\"W\",\"energy\":\"Wh\"}"));
  CHECK(strstr(info.body, "\"data_format_version\":1"));
  CHECK(strlen(info.body) < 600);

  auto expect_format = [&](int rated, const char* variant, const char* usable, const char* model_field) {
    ManualApi image(rated, variant);
    HttpReply body = image.setup_info(device, facts);
    CHECK(body.status == 200);
    char units[80];
    snprintf(units, sizeof units, "\"units\":{\"power\":\"%s\",\"energy\":\"%s\"}", powercell::kUnitlessPower,
             powercell::kUnitlessEnergy);
    char version[40];
    snprintf(version, sizeof version, "\"data_format_version\":%d", powercell::kDataFormatVersion);
    CHECK(strstr(body.body, units));
    CHECK(strstr(body.body, version));
    CHECK(strstr(body.body, "\"model\":\"esp32dev\""));
    CHECK(strstr(body.body, "\"onboarding_version\":2"));
    CHECK(strstr(body.body, "\"rated_kva\":null"));
    CHECK(strstr(body.body, usable));
    CHECK(strstr(body.body, model_field));
    CHECK(strlen(body.body) < 600);
  };
  expect_format(5000, "hw-v0.1.1-5kw", "\"usable_kw\":4.5", "\"powercell_model\":\"PC5K\"");
  expect_format(3500, "hw-v0.1.1-3.5kw", "\"usable_kw\":3.15", "\"powercell_model\":null");
  expect_format(0, "unrated", "\"usable_kw\":null", "\"powercell_model\":null");

  powercell::HomeMeters meters;
  meters.have_voltage = true;
  meters.have_current = true;
  meters.have_active = true;
  meters.active_source = "pzem004t_v3_active";
  meters.volts = 230;
  meters.amps = 0.1;
  meters.active_w = 15;
  meters.meter_present = true;
  meters.ac_valid = true;
  meters.has_lifetime_wh = true;
  meters.lifetime_wh = 1200;
  meters.has_usable = true;
  meters.usable_kw = 4.5;
  meters.powercell_model = "PC5K";
  char snapshot[2048];
  CHECK(powercell::home_snapshot_write(meters, nullptr, nullptr, snapshot, sizeof snapshot) > 0);
  CHECK(strcmp(powercell::kUnitlessPower, "W") == 0);
  CHECK(strcmp(powercell::kUnitlessEnergy, "Wh") == 0);
  CHECK(powercell::kDataFormatVersion == 1);
  CHECK(strstr(snapshot, "\"power\":15.0000"));
  CHECK(strstr(snapshot, "\"output_power_w\":15.0000"));
  CHECK(strstr(snapshot, "\"output_power\":23.0000"));
  CHECK(strstr(snapshot, "\"output_power_quantity\":\"apparent_va\""));
  CHECK(strstr(snapshot, "\"lifetime_wh\":1200.0000"));
  CHECK(strstr(snapshot, "\"usable_kw\":4.5000"));
  CHECK(strstr(snapshot, "\"power\":0.015") == nullptr);
  CHECK(strstr(snapshot, "\"lifetime_wh\":1.2") == nullptr);
  HttpReply mac = api.login(
      "{\"device_id\":\"aa:bb:cc:dd:ee:ff\",\"passcode\":\"135790\","
      "\"phone_id\":\"11111111-2222-3333-4444-555555555555\",\"phone_name\":\"Powercore\"}",
      json_headers(), device, 1000, 1700001000, 1);
  CHECK(mac.status == 401);

  uint8_t salt[16];
  memset(salt, 7, sizeof salt);
  const char* setup =
      "{\"passcode\":\"135790\",\"phone_id\":\"11111111-2222-3333-4444-555555555555\","
      "\"phone_name\":\"Powercore\"}";
  HttpReply denied = api.setup_passcode(setup, json_headers(), false, salt, 1000, 0);
  CHECK(denied.status == 403);
  HttpReply created = api.setup_passcode(setup, json_headers(), true, salt, 1000, 0);
  CHECK(created.status == 200);
  CHECK(strstr(created.body, "\"expires_in\":1800"));
  CHECK(strstr(created.body, "\"passcode_default\":false"));
  CHECK(!strstr(created.body, "\"token\":"));
  char first_lan[80];
  CHECK(field(created.body, "lan_token", first_lan, sizeof first_lan) && first_lan[0]);

  CHECK(api.login("{\"device_id\":\"aa:bb:cc:dd:ee:ff\",\"passcode\":\"135790\","
                  "\"phone_id\":\"11111111-2222-3333-4444-555555555555\",\"phone_name\":\"Powercore\"}",
                  json_headers(), device, 4000, 1700001000, 4)
            .status == 401);
  const char* login_json =
      "{\"device_id\":\"ace-000123\",\"passcode\":\"135790\","
      "\"phone_id\":\"11111111-2222-3333-4444-555555555555\",\"phone_name\":\"Powercore\"}";
  HttpReply logged = api.login(login_json, json_headers(), device, 5000, 1700001000, 1);
  CHECK(logged.status == 200);
  char session[80], lan[80];
  CHECK(field(logged.body, "session", session, sizeof session));
  CHECK(field(logged.body, "lan_token", lan, sizeof lan));
  CHECK(strcmp(session, lan) != 0);
  CHECK(strcmp(first_lan, lan) != 0);
  char bearer_session[96], bearer_lan[96];
  snprintf(bearer_session, sizeof bearer_session, "Bearer %s", session);
  snprintf(bearer_lan, sizeof bearer_lan, "Bearer %s", lan);

  HttpReply early = api.status(bearer_session, device, facts, 5000);
  CHECK(early.status == 200 && strstr(early.body, "\"auth\":\"session\""));
  HttpReply later = api.status(bearer_lan, device, facts, 5000);
  CHECK(later.status == 200 && strstr(later.body, "\"auth\":\"lan\""));
  CHECK(api.status("API-KEY secret", device, facts, 5000).status == 401);

  const char* save =
      "{\"schema\":1,\"low_percent\":30,\"warning_percent\":18,\"base_revision\":0}";
  HttpReply session_save = api.battery_post(bearer_session, save, json_headers(), 5000);
  CHECK(session_save.status == 401);
  CHECK(strstr(session_save.body, "\"reason\":\"session_not_sufficient\""));
  CHECK(strstr(session_save.body, "\"applied\":false"));
  HttpReply api_key = api.battery_post("API-KEY secret", save, json_headers(), 5000);
  CHECK(api_key.status == 401 && strstr(api_key.body, "\"reason\":\"unauthorized\""));
  HttpReply defaults = api.battery_get(bearer_lan, 4000);
  CHECK(strcmp(defaults.body,
               "{\"schema\":1,\"result\":\"readback\",\"reason\":\"none\",\"applied\":false,"
               "\"durable_commit\":true,\"pending\":false,\"physical_shutdown_verified\":false,"
               "\"readback\":{\"schema\":1,\"low_percent\":25,\"warning_percent\":15,"
               "\"inhibit_percent\":10,\"restart_percent\":15,\"applied_revision\":0,\"durable\":true}}") == 0);
  HttpReply applied = api.battery_post(bearer_lan, save, json_headers(), 5000);
  const char* applied_fixture =
      "{\"schema\":1,\"result\":\"applied\",\"reason\":\"none\",\"applied\":true,\"durable_commit\":true,"
      "\"pending\":false,\"physical_shutdown_verified\":false,"
      "\"readback\":{\"schema\":1,\"low_percent\":30,\"warning_percent\":18,\"inhibit_percent\":10,"
      "\"restart_percent\":15,\"applied_revision\":1,\"durable\":true}}";
  CHECK(applied.status == 200);
  CHECK(strcmp(applied.body, applied_fixture) == 0);
  CHECK(strstr(applied.body, "\"ceiling_w\"") == nullptr);
  HttpReply read = api.battery_get(bearer_lan, 5000);
  CHECK(read.status == 200);
  CHECK(strstr(read.body, "\"low_percent\":30"));
  CHECK(strstr(read.body, "\"warning_percent\":18"));
  CHECK(strstr(read.body, "\"applied_revision\":1"));
  CHECK(strstr(read.body, "\"inhibit_percent\":10"));
  CHECK(strstr(read.body, "\"restart_percent\":15"));
  CHECK(strstr(api.ac_get(bearer_lan, 5000).body, "\"ceiling_w\":0"));

  const char* stale =
      "{\"schema\":1,\"low_percent\":40,\"warning_percent\":20,\"base_revision\":0}";
  HttpReply stale_reply = api.battery_post(bearer_lan, stale, json_headers(), 6000);
  CHECK(stale_reply.status == 400 && strstr(stale_reply.body, "\"reason\":\"stale_revision\""));
  CHECK(strstr(api.battery_get(bearer_lan, 6000).body, "\"low_percent\":30"));

  const char* fraction =
      "{\"schema\":1,\"low_percent\":30.5,\"warning_percent\":18,\"base_revision\":1}";
  CHECK(api.battery_post(bearer_lan, fraction, json_headers(), 6000).status == 400);
  CHECK(strstr(api.battery_get(bearer_lan, 6000).body, "\"low_percent\":30"));

  const char* rating_get =
      "{\"schema\":1,\"result\":\"readback\",\"reason\":\"none\",\"applied\":false,\"durable_commit\":false,"
      "\"writable\":false,\"authority\":\"factory_image\",\"overload_configured\":true,"
      "\"physical_shutdown_verified\":false,\"factory_variant\":\"hw-v0.1.1-5kw\","
      "\"readback\":{\"rated_active_w\":5000,\"usable_active_w\":4500,\"factory_rated_active_w\":5000,"
      "\"stored_rated_active_w\":null,\"rating_match\":true}}";
  CHECK(strcmp(api.rating_get(bearer_lan, 6800).body, rating_get) == 0);
  // manual_device_client.dart 497 requires the key. Lines 505-507 reject a null
  // _optionalInt result, and 509 does not require stored.present. The helper
  // itself is not in the c874f427 extract, so a JSON null stays the truthful
  // absent NVS value. A fabricated stored integer would be a false rating.
  CHECK(strstr(rating_get, "\"stored_rated_active_w\":null"));
  CHECK(strstr(rating_get, "applied_revision") == nullptr);
  CHECK(api.rating_get(bearer_session, 6800).status == 401);
  CHECK(strstr(api.status(bearer_lan, device, facts, 6800).body, "\"factory_variant\":\"hw-v0.1.1-5kw\""));
  CHECK(strstr(api.status(bearer_lan, device, facts, 6800).body, "\"rating_writable\":false"));
  ManualImage before_write;
  api.capture(&before_write);
  HttpReply rejected = api.rating_post(
      bearer_lan, "{\"schema\":1,\"rated_active_w\":3500,\"base_revision\":0}", json_headers(), 6900);
  CHECK(rejected.status == 403);
  CHECK(strstr(rejected.body, "\"reason\":\"factory_provisioned\""));
  CHECK(strstr(rejected.body, "\"applied\":false"));
  CHECK(strstr(rejected.body, "\"durable_commit\":false"));
  CHECK(strstr(rejected.body, "\"usable_active_w\":4500"));
  CHECK(api.rating_post(bearer_lan, "{\"schema\":1,\"rated_active_w\":5000,\"base_revision\":0}", json_headers(),
                        6900)
            .status == 403);
  CHECK(api.rating_post(bearer_session, "{\"schema\":1,\"rated_active_w\":5000,\"base_revision\":0}",
                        json_headers(), 6900)
            .status == 401);
  CHECK(api.rating_post("API-KEY secret", "{\"schema\":1,\"rated_active_w\":5000,\"base_revision\":0}",
                        json_headers(), 6900)
            .status == 401);
  CHECK(strcmp(api.rating_get(bearer_lan, 6900).body, rating_get) == 0);
  ManualImage after_write;
  api.capture(&after_write);
  CHECK(after_write.rating_store_present == before_write.rating_store_present);
  CHECK(after_write.stored_rated_active_w == before_write.stored_rated_active_w);
  CHECK(api.rating().usableW() == 4500);
  HttpReply too_high = api.ac_post(
      bearer_lan, "{\"schema\":1,\"ceiling_w\":4501,\"mode\":\"warn_only\",\"base_revision\":0}", json_headers(),
      6960);
  CHECK(too_high.status == 400 && strstr(too_high.body, "\"reason\":\"ceiling_above_usable\""));
  CHECK(strstr(api.ac_get(bearer_lan, 6960).body, "\"ceiling_w\":0"));
  const char* ceiling =
      "{\"schema\":1,\"ceiling_w\":500,\"mode\":\"warn_and_shutdown\",\"base_revision\":0}";
  HttpReply ac = api.ac_post(bearer_lan, ceiling, json_headers(), 7000);
  CHECK(ac.status == 200);
  CHECK(strstr(ac.body, "\"shutdown_operational\":false"));
  CHECK(strstr(ac.body, "\"ceiling_w\":500"));
  CHECK(strstr(ac.body, "\"total_output_coverage_verified\":false"));
  HttpReply ac_session = api.ac_post(bearer_session, ceiling, json_headers(), 7000);
  CHECK(ac_session.status == 401);
  CHECK(strstr(ac_session.body, "\"reason\":\"session_not_sufficient\""));
  CHECK(strstr(ac_session.body, "\"applied\":false"));
  CHECK(strstr(api.ac_get(bearer_session, 7000).body, "\"reason\":\"session_not_sufficient\""));
  HttpReply battery_only = api.battery_post(
      bearer_lan,
      "{\"schema\":1,\"low_percent\":40,\"warning_percent\":20,\"base_revision\":1,\"ceiling_w\":0}",
      json_headers(), 7500);
  CHECK(battery_only.status == 200 && strstr(battery_only.body, "\"applied\":true"));
  CHECK(strstr(battery_only.body, "\"ceiling_w\"") == nullptr);
  CHECK(strstr(api.ac_get(bearer_lan, 7500).body, "\"ceiling_w\":500"));

  ManualImage image;
  api.capture(&image);
  ManualApi rebooted(5000, "hw-v0.1.1-5kw");
  CHECK(rebooted.restore(image));
  CHECK(rebooted.battery_get(bearer_session, 8000).status == 401);
  HttpReply restored = rebooted.battery_get(bearer_lan, 8000);
  CHECK(restored.status == 200 && strstr(restored.body, "\"low_percent\":40"));
  CHECK(strstr(rebooted.ac_get(bearer_lan, 8000).body, "\"ceiling_w\":500"));
  CHECK(strstr(rebooted.rating_get(bearer_lan, 8000).body, "\"usable_active_w\":4500"));
  CHECK(strstr(rebooted.rating_get(bearer_lan, 8000).body, "\"rated_active_w\":5000"));
  CHECK(!user_output_on_allowed(true, true));
  CHECK(user_output_on_allowed(true, false));
  ManualApi stored(5000, "hw-v0.1.1-5kw");
  image.rating_store_present = true;
  image.stored_rated_active_w = 3500;
  CHECK(stored.restore(image));
  HttpReply mismatch = stored.rating_get(bearer_lan, 8100);
  CHECK(strstr(mismatch.body, "\"reason\":\"rating_mismatch\""));
  CHECK(strstr(mismatch.body, "\"overload_configured\":false"));
  CHECK(strstr(mismatch.body, "\"usable_active_w\":null"));
  CHECK(strstr(mismatch.body, "\"factory_rated_active_w\":5000"));
  CHECK(strstr(mismatch.body, "\"stored_rated_active_w\":3500"));
  CHECK(stored.rating_post(bearer_lan, "{\"schema\":1,\"rated_active_w\":5000}", json_headers(), 8100).status == 403);
  CHECK(stored.rating().usableW() == 0);
  ManualImage mismatched;
  stored.capture(&mismatched);
  CHECK(mismatched.rating_store_present);
  CHECK(mismatched.stored_rated_active_w == 3500);
  CHECK(stored.ac_post(bearer_lan, "{\"schema\":1,\"ceiling_w\":500,\"mode\":\"warn_only\",\"base_revision\":0}",
                       json_headers(), 8100)
            .status == 400);
  image.stored_rated_active_w = 999;
  CHECK(stored.restore(image));
  CHECK(strstr(stored.rating_get(bearer_lan, 8200).body, "\"reason\":\"rating_invalid\""));
  CHECK(stored.rating().usableW() == 0);
  stored.capture(&mismatched);
  CHECK(mismatched.stored_rated_active_w == 999);
  image.rating_store_present = false;
  CHECK(stored.restore(image));
  CHECK(strcmp(stored.rating_get(bearer_lan, 8300).body, rating_get) == 0);
  CHECK(stored.rating().armed());
  CHECK(strcmp(api.rating_get(bearer_lan, 8300).body, rating_get) == 0);
  other_factory_images(device, facts);

  HttpReply bad = api.login(
      "{\"device_id\":\"ACE-000123\",\"passcode\":\"246801\","
      "\"phone_id\":\"11111111-2222-3333-4444-555555555555\",\"phone_name\":\"Powercore\"}",
      json_headers(), device, 9000, 1700001000, 2);
  CHECK(bad.status == 401 && strstr(bad.body, "\"error\":\"bad_credentials\""));
  CHECK(api.battery_get(bearer_lan, 9000).status == 200);

  ClientHeaders missing;
  missing.content_type_json = false;
  missing.accept_json = true;
  CHECK(api.login(login_json, missing, device, 9000, 1700001000, 3).status == 415);
  CHECK(api.data_auth(nullptr, 9000) == 401);
  CHECK(api.data_auth("", 9000) == 401);
  CHECK(api.data_auth(bearer_lan, 9000) == 200);
  CHECK(api.data_auth(bearer_session, 9000) == 401);
  CHECK(api.data_auth("Authorization: Bearer ota-token", 9000) == 401);

  const char* off = "{\"state\":\"off\"}";
  RouteEffect dsp = api.gateway_state(bearer_lan, off, json_headers(), OutputProfile::V011Dsp, true, false, false, 9000);
  CHECK(dsp.http.status == 200 && dsp.drive_gateway && !dsp.gateway_on);
  CHECK(strstr(dsp.http.body, "\"applied_via\":\"dsp_gpio15\""));
  CHECK(strstr(dsp.http.body, "\"gpio_level\":1"));
  CHECK(strstr(dsp.http.body, "\"aceplug\":false"));
  RouteEffect dsp_on = api.gateway_state(bearer_session, "{\"state\":\"on\"}", json_headers(),
                                         OutputProfile::V011Dsp, false, false, false, 9000);
  CHECK(dsp_on.http.status == 409 && !dsp_on.drive_gateway);
  RouteEffect plug = api.socket_relay(bearer_lan, "{\"socket_id\":\"A1B2C3D4E5F6\",\"state\":\"on\"}",
                                      json_headers(), false, true, true, false, 9000);
  CHECK(plug.http.status == 409 && !plug.queue_aceplug && !plug.drive_gateway);
  CHECK(strstr(plug.http.body, "\"error\":\"lora_not_fitted\""));
  CHECK(strstr(plug.http.body, "\"dsp_gpio15\":false"));
  RouteEffect queued = api.socket_relay(bearer_lan, "{\"socket_id\":\"A1B2C3D4E5F6\",\"state\":\"off\"}",
                                        json_headers(), true, true, true, false, 9000);
  CHECK(queued.http.status == 202 && queued.queue_aceplug && !queued.aceplug_on && !queued.drive_gateway);
  CHECK(api.gateway_state("API-KEY secret", off, json_headers(), OutputProfile::V011Dsp, true, false, false, 9000)
            .http.status == 401);
  api.set_persist_fail(true);
  HttpReply unsaved = api.battery_post(
      bearer_lan, "{\"schema\":1,\"low_percent\":42,\"warning_percent\":22,\"base_revision\":2}",
      json_headers(), 9100);
  CHECK(unsaved.status == 500);
  CHECK(strstr(unsaved.body, "\"applied\":false"));
  CHECK(strstr(unsaved.body, "\"applied\":true") == nullptr);
  CHECK(strstr(api.battery_get(bearer_lan, 9100).body, "\"low_percent\":40"));

  CHECK(!payment_allows_output(true, true, "user"));
  CHECK(!payment_allows_output(true, false, "user"));
  CHECK(payment_allows_output(true, false, "battery_critical"));
  CHECK(payment_allows_output(true, false, "ota"));
  CHECK(payment_allows_output(true, false, "ac_overload"));
  CHECK(payment_allows_output(false, true, "user"));

  struct Sink {
    int64_t system = 0;
    int system_n = 0;
    bool system_ok = true;
    int64_t rtc = 0;
    int rtc_n = 0;
    bool rtc_ok = true;
    int64_t nvs = 0;
    int nvs_tz = 0;
    int nvs_n = 0;
    bool nvs_ok = true;
  } sink;
  auto set_sys = [](int64_t epoch, void* ctx) {
    Sink* s = static_cast<Sink*>(ctx);
    if (!s->system_ok) return false;
    s->system = epoch;
    ++s->system_n;
    return true;
  };
  auto write_rtc = [](int64_t epoch, void* ctx) {
    Sink* s = static_cast<Sink*>(ctx);
    if (!s->rtc_ok) return false;
    s->rtc = epoch;
    ++s->rtc_n;
    return true;
  };
  auto persist = [](int64_t epoch, bool have_epoch, int tz, void* ctx) {
    Sink* s = static_cast<Sink*>(ctx);
    if (!s->nvs_ok) return false;
    if (have_epoch) s->nvs = epoch;
    s->nvs_tz = tz;
    ++s->nvs_n;
    return true;
  };
  ClockHooks hooks;
  hooks.set_system = set_sys;
  hooks.write_rtc = write_rtc;
  hooks.persist = persist;
  hooks.ctx = &sink;
  api.set_clock_hooks(hooks);
  CHECK(api.set_time(bearer_lan, "{\"epoch\":1700002222,\"tz_offset_min\":900}", json_headers(), 10000).status == 400);
  CHECK(sink.system_n == 0 && api.clock().now() == 0);
  HttpReply accepted = api.set_time(bearer_lan, "{\"epoch\":1700002222,\"tz_offset_min\":60}", json_headers(), 10000);
  CHECK(accepted.status == 200);
  CHECK(strstr(accepted.body, "\"applied\":true"));
  CHECK(strstr(accepted.body, "\"epoch\":1700002222"));
  CHECK(strstr(accepted.body, "\"source\":\"phone\""));
  CHECK(strstr(accepted.body, "\"rtc\":\"persisted\""));
  CHECK(strstr(accepted.body, "\"nvs_persisted\":true"));
  CHECK(sink.system == 1700002222 && sink.rtc == 1700002222 && sink.nvs == 1700002222);
  CHECK(api.clock().now() == 1700002222 && api.clock().last_good() == 1700002222);
  CHECK(api.clock().signing_trusted());
  const int after_phone = sink.system_n;
  HttpReply insync = api.set_time(bearer_lan, "{\"epoch\":1700002240}", json_headers(), 11000);
  CHECK(strstr(insync.body, "\"applied\":false"));
  CHECK(strstr(insync.body, "\"reason\":\"clock_in_sync\""));
  CHECK(strstr(insync.body, "\"rtc\":\"not_written\""));
  CHECK(sink.system_n == after_phone && api.clock().now() == 1700002222);
  const int rtc_before_ntp = sink.rtc_n;
  api.note_ntp(1700003000, 12000);
  CHECK(api.clock().source() == pc::clk::Source::Ntp);
  CHECK(api.clock().signing_trusted());
  CHECK(sink.rtc == 1700003000 && sink.rtc_n == rtc_before_ntp + 1);
  CHECK(sink.system_n == after_phone);
  HttpReply blocked = api.set_time(bearer_lan, "{\"epoch\":1700009000}", json_headers(), 13000);
  CHECK(strstr(blocked.body, "\"applied\":false"));
  CHECK(strstr(blocked.body, "\"reason\":\"ntp_authoritative\""));
  CHECK(strstr(blocked.body, "\"epoch\":1700003000"));
  CHECK(sink.system_n == after_phone && api.clock().now() == 1700003000);
  api.observe_clock(1700003000, 1, false);
  CHECK(!api.clock().signing_trusted());
  HttpReply from_nvs = api.set_time(bearer_lan, "{\"epoch\":1700004000}", json_headers(), 20000);
  CHECK(strstr(from_nvs.body, "\"applied\":true"));
  CHECK(api.clock().now() == 1700004000 && sink.system == 1700004000);
  sink.rtc_ok = false;
  HttpReply rtc_fail = api.set_time(bearer_lan, "{\"epoch\":1700005000}", json_headers(), 40000);
  CHECK(strstr(rtc_fail.body, "\"applied\":true"));
  CHECK(strstr(rtc_fail.body, "\"rtc\":\"failed\""));
  CHECK(api.clock().now() == 1700005000 && sink.system == 1700005000);
  sink.system_ok = false;
  HttpReply sys_fail = api.set_time(bearer_lan, "{\"epoch\":1700008000}", json_headers(), 80000);
  CHECK(strstr(sys_fail.body, "\"applied\":false"));
  CHECK(strstr(sys_fail.body, "\"reason\":\"clock_set_failed\""));
  CHECK(api.clock().now() == 1700005000);
  const int frozen = sink.system_n;
  CHECK(api.set_time(bearer_lan, "{\"epoch\":10}", json_headers(), 90000).status == 400);
  CHECK(sink.system_n == frozen && api.clock().now() == 1700005000);

  if (g_failures) {
    fprintf(stderr, "%d manual api checks failed\n", g_failures);
    return 1;
  }
  printf("manual api checks passed\n");
  return 0;
}
