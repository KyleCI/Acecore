#include "local_manual_api.h"
#include "output_gate.h"

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

int main() {
  DeviceNames device;
  device.requested = nullptr;
  device.mac_colon = "AA:BB:CC:DD:EE:FF";
  device.collector_ap = "Acecore PowerCell LabTest";
  device.stored_gw_name = nullptr;
  device.stored_serial = nullptr;
  UnitFacts facts;
  facts.wifi_configured = false;
  facts.model = "hw-v0.1.1";
  facts.firmware = "1.0.1";

  ManualApi api;
  api.set_random(fill_random, nullptr);
  HttpReply info = api.setup_info(device, facts);
  CHECK(info.status == 200);
  CHECK(strstr(info.body, "\"device_id\":\"ACE-DDEEFF\""));
  CHECK(strstr(info.body, "\"state\":\"unconfigured\""));
  CHECK(strstr(info.body, "\"passcode_set\":false"));
  CHECK(strstr(info.body, "\"rated_kva\":null"));
  CHECK(strstr(info.body, "\"usable_kw\":null"));

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

  const char* login_json =
      "{\"device_id\":\"aa:bb:cc:dd:ee:ff\",\"passcode\":\"135790\","
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
  CHECK(api.battery_post(bearer_session, save, json_headers(), 5000).status == 401);
  CHECK(api.battery_post("API-KEY secret", save, json_headers(), 5000).status == 401);
  HttpReply applied = api.battery_post(bearer_lan, save, json_headers(), 5000);
  CHECK(applied.status == 200);
  CHECK(strstr(applied.body, "\"result\":\"applied\""));
  CHECK(strstr(applied.body, "\"low_percent\":30"));
  CHECK(strstr(applied.body, "\"warning_percent\":18"));
  CHECK(strstr(applied.body, "\"applied_revision\":1"));
  CHECK(strstr(applied.body, "\"physical_shutdown_verified\":false"));
  HttpReply read = api.battery_get(bearer_lan, 5000);
  CHECK(read.status == 200 && strstr(read.body, "\"low_percent\":30"));

  const char* stale =
      "{\"schema\":1,\"low_percent\":40,\"warning_percent\":20,\"base_revision\":0}";
  HttpReply stale_reply = api.battery_post(bearer_lan, stale, json_headers(), 6000);
  CHECK(stale_reply.status == 400 && strstr(stale_reply.body, "\"reason\":\"stale_revision\""));
  CHECK(strstr(api.battery_get(bearer_lan, 6000).body, "\"low_percent\":30"));

  const char* fraction =
      "{\"schema\":1,\"low_percent\":30.5,\"warning_percent\":18,\"base_revision\":1}";
  CHECK(api.battery_post(bearer_lan, fraction, json_headers(), 6000).status == 400);
  CHECK(strstr(api.battery_get(bearer_lan, 6000).body, "\"low_percent\":30"));

  const char* ceiling =
      "{\"schema\":1,\"ceiling_w\":500,\"mode\":\"warn_and_shutdown\",\"base_revision\":0}";
  HttpReply ac = api.ac_post(bearer_lan, ceiling, json_headers(), 7000);
  CHECK(ac.status == 200);
  CHECK(strstr(ac.body, "\"shutdown_operational\":false"));
  CHECK(strstr(ac.body, "\"ceiling_w\":500"));
  CHECK(strstr(ac.body, "\"total_output_coverage_verified\":false"));

  ManualImage image;
  api.capture(&image);
  ManualApi rebooted;
  CHECK(rebooted.restore(image));
  CHECK(rebooted.battery_get(bearer_session, 8000).status == 401);
  HttpReply restored = rebooted.battery_get(bearer_lan, 8000);
  CHECK(restored.status == 200 && strstr(restored.body, "\"low_percent\":30"));
  CHECK(strstr(rebooted.ac_get(bearer_lan, 8000).body, "\"ceiling_w\":500"));

  HttpReply bad = api.login(
      "{\"device_id\":\"aa:bb:cc:dd:ee:ff\",\"passcode\":\"246801\","
      "\"phone_id\":\"11111111-2222-3333-4444-555555555555\",\"phone_name\":\"Powercore\"}",
      json_headers(), device, 9000, 1700001000, 2);
  CHECK(bad.status == 401 && strstr(bad.body, "\"error\":\"bad_credentials\""));
  CHECK(api.battery_get(bearer_lan, 9000).status == 200);

  ClientHeaders missing;
  missing.content_type_json = false;
  missing.accept_json = true;
  CHECK(api.login(login_json, missing, device, 9000, 1700001000, 3).status == 415);
  CHECK(api.data_auth(nullptr, 9000) == 200);
  CHECK(api.data_auth(bearer_lan, 9000) == 200);
  CHECK(api.data_auth(bearer_session, 9000) == 401);
  CHECK(api.data_auth("Authorization: Bearer ota-token", 9000) == 401);

  CHECK(!payment_allows_output(true, true, "user"));
  CHECK(!payment_allows_output(true, false, "user"));
  CHECK(payment_allows_output(true, false, "battery_critical"));
  CHECK(payment_allows_output(true, false, "ota"));
  CHECK(payment_allows_output(false, true, "user"));

  if (g_failures) {
    fprintf(stderr, "%d manual api checks failed\n", g_failures);
    return 1;
  }
  printf("manual api checks passed\n");
  return 0;
}
