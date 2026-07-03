#include "provisioning.h"

bool provisioning = false;

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
  WiFi.config(local, gateway, subnet);
  WiFi.mode(WIFI_STA);
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

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("[WiFi] Provisioning IP: ");
  Serial.println(WiFi.softAPIP());  // 192.168.4.1

  // Serve provisioning page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    String html = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>PowerCell Setup</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: sans-serif; background: #0f0f0f; color: #eee;
           display: flex; justify-content: center; align-items: center; min-height: 100vh; }
    .card { background: #1e1e1e; border-radius: 16px; padding: 32px; width: 90%; max-width: 380px; }
    h1   { color: #4fc3f7; margin-bottom: 8px; font-size: 1.3rem; }
    p    { color: #888; font-size: 0.85rem; margin-bottom: 24px; }
    label { display: block; font-size: 0.8rem; color: #aaa; margin-bottom: 6px; }
    input { width: 100%; background: #2a2a2a; border: 1px solid #333; border-radius: 8px;
            padding: 10px 14px; color: #eee; font-size: 1rem; margin-bottom: 16px; }
    input:focus { outline: none; border-color: #4fc3f7; }
    button { width: 100%; background: #4fc3f7; color: #000; border: none;
             border-radius: 8px; padding: 12px; font-size: 1rem;
             font-weight: bold; cursor: pointer; }
    button:active { opacity: 0.8; }
    #status { margin-top: 16px; text-align: center; font-size: 0.9rem; color: #4fc3f7; }
  </style>
</head>
<body>
  <div class="card">
    <h1>⚡ PowerCell Setup</h1>
    <p>Enter your WiFi credentials to connect the device to your network.</p>
    <label>Network Name (SSID)</label>
    <input type="text" id="ssid" placeholder="Your WiFi name">
    <label>Password</label>
    <input type="password" id="pass" placeholder="Your WiFi password">
    <button onclick="connect()">Connect</button>
    <div id="status"></div>
  </div>
  <script>
    function connect() {
      const ssid = document.getElementById('ssid').value.trim();
      const pass = document.getElementById('pass').value;
      if (!ssid) { document.getElementById('status').textContent = 'Please enter an SSID'; return; }
      document.getElementById('status').textContent = 'Connecting...';
      fetch('/provision?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass))
        .then(r => r.text())
        .then(t => { document.getElementById('status').textContent = t; })
        .catch(() => { document.getElementById('status').textContent = 'Error — try again'; });
    }
  </script>
</body>
</html>
    )rawhtml";
    req->send(200, "text/html", html);
  });

  // Handle credentials submission
  server.on("/provision", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("ssid"))
    {
      req->send(400, "text/plain", "Missing SSID");
      return;
    }

    String ssid = req->getParam("ssid")->value();
    String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";

    saveNVS(ssid, pass);
    req->send(200, "text/plain", "Credentials saved! Rebooting...");

    delay(1500);
    ESP.restart();
  });

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


const char PROVISION_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ACE — WiFi Setup</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=Syne:wght@700;800&display=swap');
  :root {
    --bg:#0a0b0f; --panel:#111318; --border:#1e2028;
    --accent:#00e5a0; --text:#e8eaf0; --muted:#5a5f72;
    --mono:'DM Mono',monospace; --head:'Syne',sans-serif;
    --err:#ff4560;
  }
  * { box-sizing:border-box; margin:0; padding:0; }
  body {
    background:var(--bg); color:var(--text);
    font-family:var(--mono); min-height:100vh;
    display:flex; align-items:center; justify-content:center;
  }
  .card {
    background:var(--panel); border:1px solid var(--border);
    border-radius:16px; padding:36px 32px; width:100%;
    max-width:380px;
  }
  .logo {
    font-family:var(--head); font-size:22px; font-weight:800;
    color:var(--accent); margin-bottom:4px;
  }
  .logo span { color:var(--text); }
  .subtitle {
    font-size:12px; color:var(--muted); margin-bottom:28px;
  }
  label {
    display:block; font-size:11px; color:var(--muted);
    letter-spacing:1px; text-transform:uppercase; margin-bottom:6px;
  }
  input {
    width:100%; background:#0d0e14; border:1px solid var(--border);
    border-radius:8px; padding:10px 14px; color:var(--text);
    font-family:var(--mono); font-size:14px; margin-bottom:18px;
    outline:none; transition:border-color .2s;
  }
  input:focus { border-color:var(--accent); }
  .pw-wrap { position:relative; }
  .pw-wrap input { margin-bottom:0; padding-right:44px; }
  .toggle-pw {
    position:absolute; right:12px; top:50%; transform:translateY(-50%);
    background:none; border:none; cursor:pointer;
    color:var(--muted); font-size:16px; padding:0;
  }
  .toggle-pw:hover { color:var(--text); }
  .spacer { height:18px; }
  button[type=submit] {
    width:100%; background:var(--accent); color:#000;
    border:none; border-radius:8px; padding:12px;
    font-family:var(--head); font-size:14px; font-weight:700;
    cursor:pointer; transition:opacity .15s; margin-top:24px;
    letter-spacing:1px;
  }
  button[type=submit]:hover { opacity:.85; }
  button[type=submit]:disabled { opacity:.4; cursor:not-allowed; }
  .msg {
    margin-top:16px; font-size:12px; text-align:center;
    min-height:18px; color:var(--accent);
  }
  .msg.err { color:var(--err); }
</style>
</head>
<body>
<div class="card">
  <div class="logo">ACE<span>Monitor</span></div>
  <div class="subtitle">Connect this device to your WiFi network</div>

  <label for="ssid">Network Name (SSID)</label>
  <input id="ssid" type="text" placeholder="e.g. HomeNetwork" autocomplete="off" spellcheck="false">

  <label for="pass">Password</label>
  <div class="pw-wrap">
    <input id="pass" type="password" placeholder="Leave blank if open network" autocomplete="off">
    <button class="toggle-pw" type="button" onclick="togglePw()" title="Show/hide">👁</button>
  </div>
  <div class="spacer"></div>

  <button type="submit" id="btn" onclick="submit()">Save &amp; Connect</button>
  <div class="msg" id="msg"></div>
</div>

<script>
function togglePw() {
  const p = document.getElementById('pass');
  p.type = p.type === 'password' ? 'text' : 'password';
}

function submit() {
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  const msg  = document.getElementById('msg');
  const btn  = document.getElementById('btn');

  if (!ssid) {
    msg.textContent = 'SSID cannot be empty.';
    msg.className = 'msg err';
    return;
  }

  btn.disabled = true;
  msg.className = 'msg';
  msg.textContent = 'Saving...';

  fetch('/provision?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass))
    .then(r => r.text())
    .then(t => {
      msg.textContent = '✓ ' + t;
      msg.className = 'msg';
    })
    .catch(() => {
      msg.textContent = 'Request failed — try again.';
      msg.className = 'msg err';
      btn.disabled = false;
    });
}
</script>
</body>
</html>
)rawhtml";
