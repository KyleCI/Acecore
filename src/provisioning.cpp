#include <PowercellOTA.h>
#include "provisioning.h"
#include "metrics.h"
#include "offline_sync.h"
#include <pinDefines.h>

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

// ─── Try Connecting to Saved Network ─────────────────────────────────────────
bool connectToSaved()
{
  String ssid = getNVS("ssid");
  String pass = getNVS("pass");

  if (ssid.isEmpty())
  {
    Serial.println("[WiFi] No saved network found");
    return false;
  }

  Serial.printf("[WiFi] Trying saved network: %s\n", ssid.c_str());

  IPAddress local(STATIC_IP);
  IPAddress gateway(GATEWAY);
  IPAddress subnet(SUBNET);
  // A static STA address does not inherit DNS from DHCP.  Explicit DNS is
  // required for the cloud HTTP and WebSocket hostnames to resolve.
  IPAddress dnsPrimary(DNS_PRIMARY);
  IPAddress dnsBackup(DNS_BACKUP);
  if (!WiFi.config(local, gateway, subnet, dnsPrimary, dnsBackup))
  {
    Serial.println("[WiFi] Failed to apply static IP/DNS configuration");
  }
  // Keep the AP interface enabled while connected. ESP-NOW uses the stable AP
  // MAC in both provisioning and online operation; STA still carries Internet.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - start >= CONN_TIMEOUT)
    {
      Serial.println("\n[WiFi] Connection timed out");
      WiFi.disconnect(true);
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("[WiFi] Connected! IP: ");
  Serial.println(WiFi.localIP());
  return true;
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

void applyDeviceState(bool state)
{
  if (state && firmwareOTA.busy()) return;
  deviceState = state;
  // Here you can add code to physically turn the device on or off based on the state
  digitalWrite(RELAY, deviceState ? HIGH : LOW);
  digitalWrite(LIGHTS, deviceState ? HIGH : LOW);
  Serial.printf("[Device] State changed to: %s\n", deviceState ? "ON" : "OFF");
}


void httpRoutes() {
    // ─────────────────────────────────────────────────────────────────────────
    // HTTP Routes
    // ─────────────────────────────────────────────────────────────────────────

    server.on("/ON", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        applyDeviceState(true);
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
