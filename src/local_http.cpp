#include "local_http.h"

#include "ac_output_policy.h"
#include "local_manual_api.h"
#include "offline_battery_notifications.h"
#include "provisioning.h"
#include "lora.h"
#include <PowercellOTA.h>

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

namespace {
powercell::ManualApi api;
bool begun = false;

struct Body {
  size_t len;
  bool overflow;
  char data[1024];
};

void onBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (request->_tempObject) {
      free(request->_tempObject);
      request->_tempObject = nullptr;
    }
    Body* body = static_cast<Body*>(malloc(sizeof(Body)));
    if (!body) return;
    body->len = 0;
    body->overflow = total > sizeof body->data - 1;
    request->_tempObject = body;
  }
  Body* body = static_cast<Body*>(request->_tempObject);
  if (!body || body->overflow) return;
  if (body->len + len >= sizeof body->data) {
    body->overflow = true;
    return;
  }
  memcpy(body->data + body->len, data, len);
  body->len += len;
  body->data[body->len] = 0;
}

powercell::ClientHeaders headersOf(AsyncWebServerRequest* request) {
  powercell::ClientHeaders headers;
  if (request->hasHeader("Content-Type")) {
    String type = request->header("Content-Type");
    type.toLowerCase();
    headers.content_type_json = type.indexOf("application/json") >= 0;
  }
  headers.accept_json = true;
  if (request->hasHeader("Accept")) {
    String accept = request->header("Accept");
    accept.toLowerCase();
    headers.accept_json = accept.indexOf("application/json") >= 0 || accept.indexOf("*/*") >= 0;
  }
  return headers;
}

void sendReply(AsyncWebServerRequest* request, const powercell::HttpReply& reply) {
  auto* response = request->beginResponse(reply.status, "application/json", reply.body);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void fillDevice(powercell::DeviceNames* names, char* serial, size_t serial_cap, char* gw, size_t gw_cap) {
  serial[0] = gw[0] = 0;
  Preferences prefs;
  if (prefs.begin("pc_ident", true)) {
    String value = prefs.getString("serial", "");
    prefs.end();
    snprintf(serial, serial_cap, "%s", value.c_str());
  }
  if (prefs.begin("pc_secure", true)) {
    String value = prefs.getString("gw_name", "");
    prefs.end();
    snprintf(gw, gw_cap, "%s", value.c_str());
  }
  names->requested = nullptr;
  names->mac_colon = nullptr;
  names->collector_ap = AP_SSID;
  names->stored_gw_name = gw[0] ? gw : nullptr;
  names->stored_serial = serial[0] ? serial : nullptr;
}

powercell::UnitFacts factsOf() {
  powercell::UnitFacts facts;
  facts.wifi_configured = getNVS("ssid").length() > 0;
  facts.hotspot = WiFi.softAPIP() != IPAddress(0, 0, 0, 0);
  facts.sta_connected = WiFi.status() == WL_CONNECTED;
  facts.payment_locked = false;
  Preferences prefs;
  if (prefs.begin("pc_ident", true)) {
    facts.payment_locked = prefs.isKey("pay_lock") && prefs.getBool("pay_lock", false);
    prefs.end();
  }
#ifdef POWERCELL_HW_V011
  facts.model = "hw-v0.1.1";
#else
  facts.model = "esp32dev";
#endif
  facts.firmware = "1.0.1";
  return facts;
}

bool onHotspot(AsyncWebServerRequest* request) {
  if (!request->client()) return false;
  const IPAddress ap = WiFi.softAPIP();
  return ap != IPAddress(0, 0, 0, 0) && request->client()->localIP() == ap;
}

void applySaved() {
  batteryApplyThresholds(static_cast<float>(api.battery().state().low_percent),
                         static_cast<float>(api.battery().state().warning_percent));
  const int mode = static_cast<int>(api.ac().state().mode);
  powercell::acOutputPolicy().setUsable(api.rating().state().usable_active_w);
  powercell::acOutputPolicy().setCeiling(api.ac().state().ceiling_w,
                                         mode == 1 ? powercell::AcMode::WarnAndShutdown
                                                   : powercell::AcMode::WarnOnly);
}
}  // namespace

void manualApiBegin() {
  if (begun) return;
  begun = true;
  api.set_random([](uint8_t* out, size_t n, void*) { esp_fill_random(out, n); }, nullptr);
  powercell::ManualImage image;
  memset(&image, 0, sizeof image);
  Preferences prefs;
  if (prefs.begin("pc_onb", true)) {
    String hex = prefs.getString("pass_v", "");
    snprintf(image.verifier_hex, sizeof image.verifier_hex, "%s", hex.c_str());
    image.has_passcode = hex.length() == 96;
    if (prefs.isKey("pairs")) {
      image.pairs_len = prefs.getBytesLength("pairs");
      if (image.pairs_len <= sizeof image.pairs) prefs.getBytes("pairs", image.pairs, image.pairs_len);
      else image.pairs_len = 0;
    }
    prefs.end();
  }
  if (prefs.begin("battery_cfg", true)) {
    image.low_percent = prefs.getInt("low", 25);
    image.warning_percent = prefs.getInt("warn", 15);
    image.battery_revision = prefs.getUInt("rev", 0);
    prefs.end();
  }
  if (prefs.begin("ac_cfg", true)) {
    image.ceiling_w = prefs.getInt("ceiling", 0);
    image.ac_mode = prefs.getInt("mode", 0);
    image.ac_revision = prefs.getUInt("rev", 0);
    prefs.end();
  }
  if (prefs.begin("system_cfg", true)) {
    image.rated_active_w = prefs.getInt("rated", 0);
    image.rating_revision = prefs.getUInt("rev", 0);
    prefs.end();
  }
  api.restore(image);
  api.set_persist_fail(false);
  api.set_random([](uint8_t* out, size_t n, void*) { esp_fill_random(out, n); }, nullptr);
  applySaved();
}

static bool saveImage() {
  powercell::ManualImage image;
  api.capture(&image);
  Preferences prefs;
  if (!prefs.begin("pc_onb", false)) return false;
  bool ok = true;
  if (image.has_passcode) ok = prefs.putString("pass_v", image.verifier_hex) == strlen(image.verifier_hex);
  if (ok && image.pairs_len) ok = prefs.putBytes("pairs", image.pairs, image.pairs_len) == image.pairs_len;
  prefs.end();
  if (!ok) return false;
  if (!prefs.begin("battery_cfg", false)) return false;
  ok = prefs.putInt("low", image.low_percent) == sizeof(int) &&
       prefs.putInt("warn", image.warning_percent) == sizeof(int) &&
       prefs.putUInt("rev", image.battery_revision) == sizeof(uint32_t);
  prefs.end();
  if (!ok) return false;
  if (!prefs.begin("ac_cfg", false)) return false;
  ok = prefs.putInt("ceiling", image.ceiling_w) == sizeof(int) &&
       prefs.putInt("mode", image.ac_mode) == sizeof(int) &&
       prefs.putUInt("rev", image.ac_revision) == sizeof(uint32_t);
  prefs.end();
  if (!ok) return false;
  if (!prefs.begin("system_cfg", false)) return false;
  ok = prefs.putInt("rated", image.rated_active_w) == sizeof(int) &&
       prefs.putUInt("rev", image.rating_revision) == sizeof(uint32_t);
  prefs.end();
  return ok;
}

int manualDataStatus(const char* authorization) {
  if (!begun) return 200;
  return api.data_auth(authorization, millis());
}

void manualRegister(AsyncWebServer& server) {
  server.on("/v2/setup/info", HTTP_GET, [](AsyncWebServerRequest* request) {
    char serial[32], gw[32], mac[18];
    String mac_text = WiFi.macAddress();
    snprintf(mac, sizeof mac, "%s", mac_text.c_str());
    powercell::DeviceNames names;
    fillDevice(&names, serial, sizeof serial, gw, sizeof gw);
    names.mac_colon = mac;
    sendReply(request, api.setup_info(names, factsOf()));
  });
  server.on("/v2/login", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    char serial[32], gw[32], mac[18];
    String mac_text = WiFi.macAddress();
    snprintf(mac, sizeof mac, "%s", mac_text.c_str());
    powercell::DeviceNames names;
    fillDevice(&names, serial, sizeof serial, gw, sizeof gw);
    names.mac_colon = mac;
    const uint32_t unix_s = time(nullptr) >= 1700000000 ? static_cast<uint32_t>(time(nullptr)) : 0;
    const uint32_t client = request->client() ? static_cast<uint32_t>(request->client()->remoteIP()) : 0;
    powercell::HttpReply reply = api.login(json, headersOf(request), names, millis(), unix_s, client);
    if (reply.status == 200 && !saveImage()) {
      request->send(500, "application/json", "{\"error\":\"storage_failed\"}");
      return;
    }
    sendReply(request, reply);
  }, nullptr, onBody);
  server.on("/v2/setup/passcode", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    uint8_t salt[16];
    esp_fill_random(salt, sizeof salt);
    powercell::HttpReply reply =
        api.setup_passcode(json, headersOf(request), onHotspot(request), salt, millis(), 0);
    if (reply.status == 200 && !saveImage()) {
      request->send(500, "application/json", "{\"error\":\"storage_failed\"}");
      return;
    }
    sendReply(request, reply);
  }, nullptr, onBody);
  server.on("/v2/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    char serial[32], gw[32], mac[18];
    String mac_text = WiFi.macAddress();
    snprintf(mac, sizeof mac, "%s", mac_text.c_str());
    powercell::DeviceNames names;
    fillDevice(&names, serial, sizeof serial, gw, sizeof gw);
    names.mac_colon = mac;
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    sendReply(request, api.status(auth.c_str(), names, factsOf(), millis()));
  });
  server.on("/v1/battery-settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    sendReply(request, api.battery_get(auth.c_str(), millis()));
  });
  server.on("/v1/battery-settings", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    powercell::HttpReply reply = api.battery_post(auth.c_str(), json, headersOf(request), millis());
    if (reply.status == 200 && !saveImage()) {
      request->send(500, "application/json",
                    "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"persist_failed\",\"applied\":false}");
      return;
    }
    if (reply.status == 200) applySaved();
    sendReply(request, reply);
  }, nullptr, onBody);
  server.on("/v1/ac-output-settings", HTTP_GET, [](AsyncWebServerRequest* request) {
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    sendReply(request, api.ac_get(auth.c_str(), millis()));
  });
  server.on("/v1/ac-output-settings", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    powercell::HttpReply reply = api.ac_post(auth.c_str(), json, headersOf(request), millis());
    if (reply.status == 200 && !saveImage()) {
      request->send(500, "application/json",
                    "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"persist_failed\",\"applied\":false,"
                    "\"shutdown_operational\":false}");
      return;
    }
    if (reply.status == 200) applySaved();
    sendReply(request, reply);
  }, nullptr, onBody);
  server.on("/v1/system-rating", HTTP_GET, [](AsyncWebServerRequest* request) {
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    sendReply(request, api.rating_get(auth.c_str(), millis()));
  });
  server.on("/v1/system-rating", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"error\":\"body_too_large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
    powercell::HttpReply reply = api.rating_post(auth.c_str(), json, headersOf(request), millis());
    if (reply.status == 200 && !saveImage()) {
      request->send(500, "application/json",
                    "{\"schema\":1,\"result\":\"rejected\",\"reason\":\"persist_failed\",\"applied\":false}");
      return;
    }
    if (reply.status == 200) applySaved();
    sendReply(request, reply);
  }, nullptr, onBody);
  server.on("/state", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"ok\":false,\"error\":\"body too large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
#ifdef POWERCELL_HW_V011
    const powercell::OutputProfile profile = powercell::OutputProfile::V011Dsp;
#else
    const powercell::OutputProfile profile = powercell::OutputProfile::OriginalRelay;
#endif
    powercell::RouteEffect effect = api.gateway_state(auth.c_str(), json, headersOf(request), profile,
                                                      batteryOutputOnAllowed(), firmwareOTA.busy(),
                                                      paymentLockEngaged(), millis());
    if (effect.drive_gateway) applyDeviceState(effect.gateway_on, "user");
    sendReply(request, effect.http);
  }, nullptr, onBody);
  server.on("/relay", HTTP_POST, [](AsyncWebServerRequest* request) {
    Body* stored = static_cast<Body*>(request->_tempObject);
    if (stored && stored->overflow) {
      request->send(413, "application/json", "{\"ok\":false,\"error\":\"body too large\"}");
      return;
    }
    const char* json = stored ? stored->data : "";
    String auth = request->hasHeader("Authorization") ? request->header("Authorization") : String();
#ifdef POWERCELL_HW_V011
    const bool lora = false;
    const bool registered = false;
#else
    const bool lora = true;
    String sid;
    const char* idp = strstr(json, "\"socket_id\":\"");
    if (!idp) idp = strstr(json, "\"id\":\"");
    if (idp) {
      idp = strchr(idp, ':');
      if (idp) {
        ++idp;
        while (*idp == ' ' || *idp == '"') ++idp;
        char tmp[40];
        size_t n = 0;
        while (*idp && *idp != '"' && n + 1 < sizeof tmp) tmp[n++] = *idp++;
        tmp[n] = 0;
        sid = tmp;
      }
    }
    const bool registered = sid.length() && checkRegistered(sid);
#endif
    powercell::RouteEffect effect = api.socket_relay(auth.c_str(), json, headersOf(request), lora, registered,
                                                     batteryOutputOnAllowed(), paymentLockEngaged(), millis());
    if (effect.queue_aceplug) sendRelayCommand(String(effect.socket_id), effect.aceplug_on);
    sendReply(request, effect.http);
  }, nullptr, onBody);
}
