#include <PowercellOTA.h>
#include "provisioning.h"
#include "metrics.h"
#include "offline_sync.h"
#include <pinDefines.h>
#include "wifi_fallback.h"
#include "offline_battery_notifications.h"
#include "battery_settings.h"
#ifdef POWERCELL_HW_V011
#include "board_pins.h"
#include "hw_v011.h"
#endif

bool provisioning = false;
bool shouldRestart = false;
bool deviceState = false;

// ─── Globals ──────────────────────────────────────────────────────────────────
AsyncWebServer server(80);
Preferences    prefs;


// ─── NVS Helpers ─────────────────────────────────────────────────────────────
String getNVS(const char *key)
{
  prefs.begin("wifi", true);   // true = read-only
  String val = prefs.getString(key, "");
  prefs.end();
  return val;
}

void saveNVS(const String &ssid, const String &pass)
{
  prefs.begin("wifi", false);  // false = read-write
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearNVS()
{
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

// ─── Station / local-AP lifecycle ────────────────────────────────────────────
static powercell::WifiFallback g_wifi;
static bool g_wifi_ready = false;
static bool g_sta_started = false;
static bool g_ap_started = false;
static String g_ssid;
static String g_pass;
static uint32_t g_provision_activity_ms = 0;

static void noteProvisionActivity() { g_provision_activity_ms = millis(); }

static void applyStaticStaConfig()
{
  IPAddress local(STATIC_IP);
  IPAddress gateway(GATEWAY);
  IPAddress subnet(SUBNET);
  IPAddress dnsPrimary(DNS_PRIMARY);
  IPAddress dnsBackup(DNS_BACKUP);
  if (!WiFi.config(local, gateway, subnet, dnsPrimary, dnsBackup))
  {
    Serial.println("[WiFi] Failed to apply static IP/DNS configuration");
  }
}

void beginWifiService()
{
  if (g_wifi_ready) return;
  g_ssid = getNVS("ssid");
  g_pass = getNVS("pass");
  g_wifi.set_credentials(g_ssid.length() > 0);
  // AP+STA stays selected. Mode is not flipped on later retries.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  if (g_ssid.length() > 0)
  {
    applyStaticStaConfig();
    Serial.printf("[WiFi] Trying saved network: %s\n", g_ssid.c_str());
  }
  else
  {
    Serial.println("[WiFi] No saved network found");
  }
  g_wifi_ready = true;
  serviceWifiFallback();
}

void serviceWifiFallback()
{
  if (!g_wifi_ready) return;
  const bool station_joined = WiFi.softAPgetStationNum() > 0;
  const bool provision_recent =
      g_provision_activity_ms != 0 &&
      static_cast<uint32_t>(millis() - g_provision_activity_ms) < 120000;
  g_wifi.set_onboarding(station_joined || provision_recent);

  const powercell::WifiSta sta =
      WiFi.status() == WL_CONNECTED ? powercell::WifiSta::Up : powercell::WifiSta::Down;
  const powercell::WifiCommand cmd = g_wifi.tick(sta, millis());

  if (cmd.start_ap && !g_ap_started)
  {
    WiFi.softAP(AP_SSID, AP_PASS);
    g_ap_started = true;
    Serial.printf("[WiFi] Local AP up at %s\n", WiFi.softAPIP().toString().c_str());
  }
  if (cmd.stop_ap && g_ap_started)
  {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_AP_STA);
    g_ap_started = false;
    Serial.println("[WiFi] Local AP stopped after recovery grace");
  }
  if (cmd.reconnect_sta && g_ssid.length() > 0)
  {
    if (!g_sta_started)
    {
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());
      g_sta_started = true;
    }
    else
    {
      WiFi.reconnect();
    }
  }
  if (cmd.local_mode) provisioning = true;
  else if (!station_joined && !provision_recent) provisioning = false;

  if (WiFi.status() == WL_CONNECTED) initWebSocketCommands();
}

bool connectToSaved()
{
  beginWifiService();
  return WiFi.status() == WL_CONNECTED;
}

// ─── Provisioning AP + Page ──────────────────────────────────────────────────
void startProvisioning()
{
  provisioning = true;
  Serial.println("[WiFi] Starting provisioning AP...");

  // Keep both interfaces enabled so the ESP-NOW AP identity remains available
  // before and after router provisioning.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("[WiFi] Provisioning IP: ");
  Serial.println(WiFi.softAPIP());  // 192.168.4.1

  // Routes are registered once by httpRoutes(); /provision is served from
  // SPIFFS so every client receives the same tested page.
  server.begin();
}

// ─── Main App Server ──────────────────────────────────────────────────────────
void startAppServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/home.html", "text/html");
  });

  server.on("/home.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/home.html", "text/html");
  });

  // Add your /metrics, /ON, /OFF routes here...

  // Catch-all for SPIFFS files
  server.onNotFound([](AsyncWebServerRequest *req) {
    String path = req->url();
    if (SPIFFS.exists(path))
    {
      String ct = "text/plain";
      if (path.endsWith(".html")) ct = "text/html";
      else if (path.endsWith(".css"))  ct = "text/css";
      else if (path.endsWith(".js"))   ct = "application/javascript";
      else if (path.endsWith(".png"))  ct = "image/png";
      req->send(SPIFFS, path, ct);
    }
    else
    {
      req->send(404, "text/plain", "Not found");
    }
  });

  server.begin();
  Serial.println("[HTTP] App server started at http://192.168.1.100");
}

bool applyDeviceState(bool state)
{
  if (state && firmwareOTA.busy()) return false;
  if (state && !batteryOutputOnAllowed()) {
    Serial.println("[Battery] On blocked until the battery is at least 15% and you turn it on");
    return false;
  }
  deviceState = state;
#ifdef POWERCELL_HW_V011
  digitalWrite(powercell::kDspEnableV011, powercell::v011_dsp_level(deviceState));
  hwV011NoteState(deviceState);
#else
  digitalWrite(RELAY, deviceState ? HIGH : LOW);
  digitalWrite(LIGHTS, deviceState ? HIGH : LOW);
#endif
  Serial.printf("[Device] State changed to: %s\n", deviceState ? "ON" : "OFF");
  return true;
}


static void sendBatterySettings(AsyncWebServerRequest* req, int code,
                                const powercell::SettingsDecision& decision, bool include_readback)
{
  JsonDocument doc;
  doc["schema"] = 1;
  doc["result"] = decision.applied ? "applied" : "rejected";
  doc["reason"] = powercell::settings_reason_name(decision.reason);
  doc["applied"] = decision.applied;
  doc["durable_commit"] = decision.durable_commit;
  doc["pending"] = false;
  doc["physical_shutdown_verified"] = false;
  doc["actuator_semantics"] = "user_confirmed";
  doc["cutoff_stops_charging"] = false;
  doc["payment_lock_actuator"] = false;
#ifdef POWERCELL_HW_V011
  doc["physical_auto_off"] = "implemented_ac_output_only";
  doc["output_authority"] = "v011_dsp_active_low_user_confirmed";
  doc["collector_profile"] = "hw-v0.1.1";
#else
  doc["physical_auto_off"] = "disabled";
  doc["output_authority"] = "software_inhibit_only";
  doc["collector_profile"] = "original";
#endif
  doc["authenticated_configuration"] = false;
  doc["settings_save_complete"] = false;
  doc["collector_source"] = "KyleCI/Acecore";
  doc["lan_token_issued"] = false;
  doc["deployed_2_5_11_compatible"] = false;
  if (include_readback) {
    JsonObject readback = doc["readback"].to<JsonObject>();
    readback["schema"] = decision.readback.schema;
    readback["low_percent"] = decision.readback.low_percent;
    readback["warning_percent"] = decision.readback.warning_percent;
    readback["inhibit_percent"] = decision.readback.inhibit_percent;
    readback["restart_percent"] = decision.readback.restart_percent;
    readback["applied_revision"] = decision.readback.applied_revision;
    readback["durable"] = decision.readback.durable;
    readback["physical_shutdown_verified"] = false;
    readback["bms_own_soc_hold"] = true;
    readback["charging_evidence_required"] = false;
  } else {
    doc["readback_present"] = false;
  }
  String body;
  serializeJson(doc, body);
  req->send(code, "application/json", body);
}

static powercell::SettingsAuth lanAuthorization(AsyncWebServerRequest* req)
{
  if (!req->hasHeader("Authorization")) return powercell::SettingsAuth::Missing;
  String header = req->header("Authorization");
  header.trim();
  const char prefix[] = "Bearer ";
  if (!header.startsWith(prefix)) return powercell::SettingsAuth::Malformed;
  String token = header.substring(sizeof(prefix) - 1);
  token.trim();
  if (!token.length()) return powercell::SettingsAuth::Missing;
  // This source does not issue or store a per-phone LAN token. A presented
  // Bearer token is recognized and rejected. It is not compared to the OTA token.
  return powercell::SettingsAuth::LanTokenUnsupported;
}

static void rejectBatterySettings(AsyncWebServerRequest* req)
{
  powercell::BatterySettings unused;
  const powercell::SettingsDecision decision =
      unused.set(lanAuthorization(req), 1, true, 25, true, 15, 0, false);
  sendBatterySettings(req, 401, decision, false);
}

void httpRoutes() {
#ifdef POWERCELL_HW_V011
    hwV011Routes(server);
#endif
    auto settingsReject = [](AsyncWebServerRequest* req) { rejectBatterySettings(req); };
    server.on("/v1/battery-settings", HTTP_GET, settingsReject);
    server.on("/v1/battery-settings", HTTP_POST, settingsReject);
    // ─────────────────────────────────────────────────────────────────────────
    // HTTP Routes
    // ─────────────────────────────────────────────────────────────────────────

    server.on("/ON", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (!applyDeviceState(true)) {
            req->send(409, "text/plain", "battery_output_inhibited");
            return;
        }
        queueOfflineEvent("gateway_state", "", "on");

        req->send(
            200,
            "text/plain",
            "ON"
        );
    });


    server.on("/OFF", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        applyDeviceState(false);
        queueOfflineEvent("gateway_state", "", "off");

        req->send(
            200,
            "text/plain",
            "OFF"
        );
    });


    // ─────────────────────────────────────────────────────────────────────────
    // Provisioning
    // ─────────────────────────────────────────────────────────────────────────

    server.on("/provision", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        noteProvisionActivity();
        if (req->hasParam("ssid"))
        {
            String ssid =
                req->getParam("ssid")->value();

            String pass =
                req->hasParam("pass")
                    ? req->getParam("pass")->value()
                    : "";

            saveNVS(ssid, pass);

            req->send(
                200,
                "text/plain",
                "Saved! Rebooting..."
            );

            shouldRestart = true;
        }
        else
        {
             req->send(
            SPIFFS,
            "/provision.html",
            "text/html"
        );
        }
    });


    // ─────────────────────────────────────────────────────────────────────────
    // Main Pages
    // ─────────────────────────────────────────────────────────────────────────

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (!SPIFFS.exists("/home.html"))
        {
            req->send(503, "text/plain", "Web interface is not installed. Upload the SPIFFS filesystem with: pio run --target uploadfs");
            return;
        }
        req->send(
            SPIFFS,
            "/home.html",
            "text/html"
        );
    });


    server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (!SPIFFS.exists("/smart_socket.html"))
        {
            req->send(503, "text/plain", "Device interface is not installed. Upload the SPIFFS filesystem with: pio run --target uploadfs");
            return;
        }
        req->send(
            SPIFFS,
            "/smart_socket.html",
            "text/html"
        );
    });


    server.on("/smart_socket.html", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        req->send(
            SPIFFS,
            "/smart_socket.html",
            "text/html"
        );
    });


    // ─────────────────────────────────────────────────────────────────────────
    // Device State + Metrics
    // ─────────────────────────────────────────────────────────────────────────

    server.on("/state", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        req->send(
            200,
            "text/plain",
            deviceState ? "ON" : "OFF"
        );
    });


    registerMetricsRoute(server);   // BMS + LoRa -> /metrics (see metrics.cpp)


    // ─────────────────────────────────────────────────────────────────────────
    // Smart Socket Control (LoRa) — deviceId comes from Smart Socket's own
    // MAC (colons stripped), same string /metrics reports back in "sockets"
    // ─────────────────────────────────────────────────────────────────────────

    server.on("/relay", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (!req->hasParam("id") || !req->hasParam("state"))
        {
            req->send(400, "text/plain", "id and state params required");
            return;
        }

        String deviceId = req->getParam("id")->value();
        String state    = req->getParam("state")->value();
        bool turnOn = state.equalsIgnoreCase("on") || state == "1";

        sendRelayCommand(deviceId, turnOn);

        req->send(
            200,
            "text/plain",
            turnOn ? "ON" : "OFF"
        );
    });


    server.on("/register", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (!req->hasParam("id"))
        {
            req->send(400, "text/plain", "id param required");
            return;
        }

        String socketId = req->getParam("id")->value();
        registerDevice(socketId);
        queueOfflineEvent("socket_paired", socketId);

        req->send(
            200,
            "text/plain",
            "Registered"
        );
    });


    server.on("/unregister", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (!req->hasParam("id"))
        {
            req->send(400, "text/plain", "id param required");
            return;
        }

        String socketId = req->getParam("id")->value();
        unregisterDevice(socketId);
        queueOfflineEvent("socket_forgotten", socketId);

        req->send(
            200,
            "text/plain",
            "Unregistered"
        );
    });


    // // ─────────────────────────────────────────────────────────────────────────
    // // Monitor Routes
    // // ─────────────────────────────────────────────────────────────────────────

    // server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest *req)
    // {
    //     handleRoot(req);
    // });


    // server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req)
    // {
    //     handleData(req);
    // });


    // server.on("/approve", HTTP_GET, [](AsyncWebServerRequest *req)
    // {
    //     handleApprove(req);
    // });


    // server.on("/reject", HTTP_GET, [](AsyncWebServerRequest *req)
    // {
    //     handleReject(req);
    // });


    // server.on("/relay", HTTP_GET, [](AsyncWebServerRequest *req)
    // {
    //     handleRelay(req);
    // });


    // // ─────────────────────────────────────────────────────────────────────────
    // // Server-Sent Events
    // // ─────────────────────────────────────────────────────────────────────────

    // events.onConnect([](AsyncEventSourceClient *client)
    // {
    //     client->send("{\"status\":\"connected\"}", "ready", millis(), 1000);
    //     String state = buildJSON();
    //     client->send(state.c_str(), "state", millis(), 1000);
    // });
    // server.addHandler(&events);


    // ─────────────────────────────────────────────────────────────────────────
    // Static Files / 404
    // ─────────────────────────────────────────────────────────────────────────

    server.onNotFound([](AsyncWebServerRequest *req)
    {
        String path = req->url();

        if (SPIFFS.exists(path))
        {
            String contentType = "text/plain";

            if (path.endsWith(".html"))
                contentType = "text/html";

            else if (path.endsWith(".css"))
                contentType = "text/css";

            else if (path.endsWith(".js"))
                contentType = "application/javascript";

            else if (path.endsWith(".png"))
                contentType = "image/png";

            else if (path.endsWith(".jpg"))
                contentType = "image/jpeg";

            else if (path.endsWith(".jpeg"))
                contentType = "image/jpeg";

            else if (path.endsWith(".svg"))
                contentType = "image/svg+xml";

            req->send(
                SPIFFS,
                path,
                contentType
            );
        }
        else
        {
            req->send(
                404,
                "text/plain",
                "Not found"
            );
        }
    });

}
