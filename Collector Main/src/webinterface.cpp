// ============================================================
//  webinterface.cpp  — Sensor data, Dashboard HTML (PROGMEM),
//                      JSON builder, and all HTTP route handlers.
// ============================================================

#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#include "webinterface.h"
#include "adcreadings.h"
#include "mppt.h"
#include "sensor_task.h"
#include "spi_task.h"
#include "device_manager.h"
#include "espnow_handler.h"
#include "config.h"
#include "provisioning.h"

float batteryVoltage = 0.0;
float outEnergy = 0.0;
float outPower = 0.0;
float batPercent = 0.0;
String mppt_values;

// ── Shared server instance ────────────────────────────────────────────────────
AsyncEventSource events("/events");

// ── Device / sensor state ─────────────────────────────────────────────────────
bool deviceState        = false;   // false = OFF, true = ON
unsigned long lastUpdate = 0;

// Sensor shadow values (updated by getSensorData())
float power_drawn         = 7.70;
float energy_consumed     = 0.13;
float battery_voltage     = 3.85;
int   battery_percentage  = 65;
float battery_temperature = 25.50;
float system_temperature  = 30.20;
float solar_panel_voltage = 12.50;



// ============================================================
//  Persistent device-state helpers
// ============================================================
void saveDeviceState() {
    File file = SPIFFS.open("/state.txt", "w");
    if (!file) {
        Serial.println("Failed to open state.txt for writing");
        return;
    }
    file.print(deviceState ? "ON" : "OFF");
    file.close();
}

bool loadDeviceState() {
    File file = SPIFFS.open("/state.txt", "r");
    if (!file) {
        Serial.println("Failed to open state.txt; defaulting to OFF");
        return false;
    }
    String state = file.readStringUntil('\n');
    file.close();
    return (state == "ON");
}


// ============================================================
//  Sensor sampling + local JSON payload
// ============================================================
String getSensorData() {
  // Read the cached snapshot published by the background sensor task.
  // No ADC/PZEM work happens on the request path anymore.
  SensorCache s = getSensorCache();

  JsonDocument doc;                       // v7
  doc["device_state"]        = deviceState ? "On" : "Off";
  doc["battery_voltage"]     = s.batVoltage;
  doc["battery_percentage"]  = s.batteryPercent;
  doc["inverter_output_load"]         = s.output.power;      // was outPower, always 0.0
  doc["inverter_temperature"]  = system_temperature;
  doc["inverter_energy"]     = s.output.energy;     // was outEnergy, always 0.0

  // Cached MPPT frame (parsed in the background SPI task). Emits the parsed
  // object when fresh (< 5 s old), otherwise null.
  String mppt = getMpptJson(5000);
  if (mppt.length()) doc["mppt_data"] = serialized(mppt);
  else               doc["mppt_data"] = nullptr;

  String out;
  serializeJson(doc, out);
  return out;
}



// ============================================================
//  DASHBOARD HTML  (stored in flash to save DRAM)
// ============================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ACE Power Monitor</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=Syne:wght@700;800&display=swap');
  :root {
    --bg:#0a0b0f; --panel:#111318; --border:#1e2028;
    --accent:#00e5a0; --warn:#ffd166; --blue:#74b9ff;
    --text:#e8eaf0; --muted:#5a5f72;
    --on:#00e5a0; --off:#ff4560;
    --mono:'DM Mono',monospace; --head:'Syne',sans-serif;
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);font-family:var(--mono);min-height:100vh;padding-bottom:60px}

  /* TOPBAR */
  .topbar{display:flex;align-items:center;justify-content:space-between;padding:16px 24px;border-bottom:1px solid var(--border);background:var(--panel);position:sticky;top:0;z-index:100}
  .logo{font-family:var(--head);font-size:18px;font-weight:800;color:var(--accent)}
  .logo span{color:var(--text)}
  .topbar-right{display:flex;align-items:center;gap:16px;font-size:12px;color:var(--muted)}
  .devname-badge{background:#1a2a1a;border:1px solid #2a4a2a;color:var(--accent);font-size:11px;padding:3px 10px;border-radius:20px;letter-spacing:1px}
  .status-dot{width:8px;height:8px;border-radius:50%;background:var(--accent);display:inline-block;margin-right:6px;animation:pulse 2s infinite}
  .status-dot.off{background:var(--off);animation:none}
  @keyframes pulse{0%,100%{opacity:1;box-shadow:0 0 0 0 rgba(0,229,160,.4)}50%{opacity:.7;box-shadow:0 0 0 6px rgba(0,229,160,0)}}

  /* PENDING BANNER */
  .pending-banner{background:linear-gradient(90deg,#1a1200,#1a0a00);border-bottom:1px solid #3a2800;padding:12px 24px;display:flex;align-items:center;gap:14px;font-size:13px}
  .pending-banner .tag{background:var(--warn);color:#000;font-family:var(--head);font-size:10px;font-weight:700;padding:3px 8px;border-radius:4px;letter-spacing:1px}
  .pending-banner .mac{color:var(--warn)}
  .btn{padding:7px 16px;border-radius:6px;border:none;cursor:pointer;font-family:var(--mono);font-size:12px;transition:opacity .15s}
  .btn:hover{opacity:.8}
  .btn-approve{background:var(--accent);color:#000}
  .btn-reject{background:transparent;color:var(--off);border:1px solid var(--off)}

  /* GRID */
  .main{padding:20px 24px}
  .section-label{font-family:var(--head);font-size:11px;letter-spacing:2px;color:var(--muted);text-transform:uppercase;margin-bottom:14px}
  .devices-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:14px;margin-bottom:28px}

  /* CARD BASE */
  .card{background:var(--panel);border:1px solid var(--border);border-radius:12px;overflow:hidden;transition:border-color .2s,opacity .3s}
  .card:hover{border-color:#2e3140}
  .card.stale{opacity:.5}
  .card.tripped{border-color:rgba(255,69,96,0.5);box-shadow:0 0 20px rgba(255,69,96,0.08)}
  .card-header{padding:14px 16px;border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between}
  .card-title{font-family:var(--head);font-size:14px;font-weight:700}
  .card-sub{font-size:10px;color:var(--muted);margin-top:2px}
  .card-body{padding:16px}
  .card-footer{padding:8px 16px;border-top:1px solid var(--border);font-size:10px;color:var(--muted);display:flex;justify-content:space-between}

  /* OVERCURRENT ALERT BANNER */
  .oc-alert{
    display:flex;align-items:center;gap:8px;
    background:rgba(255,69,96,0.1);
    border:1px solid rgba(255,69,96,0.35);
    color:#ff4560;border-radius:8px;
    padding:8px 12px;font-size:11px;
    margin-bottom:12px;font-weight:500;
    animation:ocpulse 1s infinite;
  }
  .oc-alert .oc-icon{font-size:16px;flex-shrink:0}
  .oc-alert .oc-text{line-height:1.4}
  .oc-alert .oc-amps{font-size:16px;font-weight:500;margin-left:auto;color:#ff4560}
  @keyframes ocpulse{0%,100%{border-color:rgba(255,69,96,0.35)}50%{border-color:rgba(255,69,96,0.7)}}

  /* METRICS */
  .metrics{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:12px}
  .metric{background:#0d0e14;border-radius:8px;padding:10px 8px;text-align:center}
  .metric-val{font-size:20px;font-weight:500;line-height:1;margin-bottom:3px}
  .metric-val.v{color:var(--warn)}
  .metric-val.i{color:var(--blue)}
  .metric-val.p{color:var(--accent)}
  .metric-unit{font-size:9px;color:var(--muted);letter-spacing:1px;text-transform:uppercase}

  /* SPARKLINE */
  .sparkline-wrap{position:relative;height:36px;margin-top:6px}
  .sparkline{width:100%;height:36px;display:block}
  .spark-label{position:absolute;top:2px;right:0;font-size:9px;color:var(--muted)}

  /* RELAY TOGGLE */
  .relay-toggle{display:flex;align-items:center;gap:8px}
  .relay-label{font-size:11px;font-weight:500;letter-spacing:1px;text-transform:uppercase;min-width:36px;text-align:right}
  .relay-label.on{color:var(--on)}
  .relay-label.off{color:var(--off)}
  .relay-label.pending{color:var(--warn);animation:blink .7s infinite}
  .relay-label.trip{color:#ff4560;animation:blink .5s infinite}
  @keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
  .switch{position:relative;display:inline-block;width:42px;height:22px}
  .switch input{opacity:0;width:0;height:0}
  .slider{position:absolute;inset:0;background:#2a2d38;border-radius:22px;cursor:pointer;transition:background .25s}
  .slider:before{content:'';position:absolute;width:16px;height:16px;left:3px;top:3px;background:#fff;border-radius:50%;transition:transform .25s}
  input:checked+.slider{background:var(--on)}
  input:checked+.slider:before{transform:translateX(20px)}
  .switch.pending .slider,.switch.tripped .slider{opacity:.45;cursor:not-allowed}
  .switch.pending input,.switch.tripped input{pointer-events:none}

  /* EMPTY */
  .empty{text-align:center;padding:50px 20px;color:var(--muted)}
  .empty-icon{font-size:36px;margin-bottom:10px}
  .empty-title{font-family:var(--head);font-size:16px;color:var(--text);margin-bottom:4px}

  /* FOOTER */
  .footer-stats{display:flex;gap:20px;padding:14px 24px;border-top:1px solid var(--border);background:var(--panel);font-size:12px;color:var(--muted);position:fixed;bottom:0;left:0;right:0}
  .footer-stats strong{color:var(--accent)}

  @keyframes flashIn{0%{background:rgba(0,229,160,.1)}100%{background:transparent}}
  .flash{animation:flashIn .4s ease}
  @keyframes flashOC{0%{background:rgba(255,69,96,.15)}100%{background:transparent}}
  .flash-oc{animation:flashOC .4s ease}
</style>
</head>
<body>
<div class="topbar">
  <div class="logo">ACE<span>Monitor</span></div>
  <div class="topbar-right">
    <span class="devname-badge" id="devname">—</span>
    <span><span class="status-dot" id="dot"></span><span id="conn-label">Connecting...</span></span>
    <span id="clock">--:--:--</span>
  </div>
</div>

<div id="pending-area"></div>

<div class="main">
  <div class="section-label">Live Devices</div>
  <div class="devices-grid" id="grid">
    <div class="empty">
      <div class="empty-icon">📡</div>
      <div class="empty-title">No devices yet</div>
      <div>Waiting for sockets to connect...</div>
    </div>
  </div>
</div>

<div class="footer-stats">
  <span>Devices: <strong id="f-dev">0</strong></span>
  <span>Overcurrent: <strong id="f-oc" style="color:#ff4560">0</strong></span>
  <span>Last update: <strong id="f-upd">—</strong></span>
</div>

<script>
const pwrHistory = {}, MAX_HIST = 30;

function addHist(id, p) {
  if (!pwrHistory[id]) pwrHistory[id] = [];
  pwrHistory[id].push(p);
  if (pwrHistory[id].length > MAX_HIST) pwrHistory[id].shift();
}

function drawSpark(id, data, color) {
  const c = document.getElementById('spark-' + id);
  if (!c) return;
  const ctx = c.getContext('2d');
  c.width = c.offsetWidth;
  ctx.clearRect(0, 0, c.width, c.height);
  if (data.length < 2) return;
  const mn = Math.min(...data), mx = Math.max(...data), rng = mx - mn || 1;
  const w = c.width, h = c.height;
  ctx.beginPath();
  data.forEach((v, i) => {
    const x = (i / (data.length - 1)) * w;
    const y = h - ((v - mn) / rng) * (h - 4) - 2;
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  });
  ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.stroke();
  ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
  ctx.fillStyle = color + '22'; ctx.fill();
}

function relayToggle(devId, checked) {
  fetch('/relay?dev=' + devId + '&state=' + (checked ? 1 : 0))
    .catch(() => console.error('Relay fetch failed'));
}

function approve() {
  fetch('/approve').then(() => poll()).catch(() => {});
}

function reject() {
  fetch('/reject').then(() => poll()).catch(() => {});
}

function setConnected(v) {
  document.getElementById('dot').className = 'status-dot' + (v ? '' : ' off');
  document.getElementById('conn-label').textContent = v ? 'Live' : 'Reconnecting...';
}

function socketCardHTML(d) {
  const stale  = d.age > 15;
  const isOC   = d.overcurrent === 1;
  const isPend = d.pending === 1 && !isOC;

  let rlLabel, swClass;
  if (isOC) {
    rlLabel = `<span class="relay-label trip">TRIP</span>`;
    swClass = 'switch tripped';
  } else if (isPend) {
    rlLabel = `<span class="relay-label pending">WAIT</span>`;
    swClass = 'switch pending';
  } else {
    rlLabel = `<span class="relay-label ${d.relay ? 'on' : 'off'}">${d.relay ? 'ON' : 'OFF'}</span>`;
    swClass = 'switch';
  }

  const ocBanner = isOC ? `
    <div class="oc-alert">
      <span class="oc-icon">⚡</span>
      <span class="oc-text">OVERCURRENT — Relay auto-tripped<br>
        <span style="font-size:9px;color:#ff456099">Relay will restore when current drops below ${(d.oc_amps || 0).toFixed(1)} A</span>
      </span>
      <span class="oc-amps">${(d.oc_amps || 0).toFixed(2)} A</span>
    </div>` : '';

  const iColor = isOC ? '#ff4560' : '#74b9ff';

  return {
    cls: 'card' + (stale ? ' stale' : '') + (isOC ? ' tripped' : ''),
    html: `
    <div class="card-header">
      <div>
        <div class="card-title">${d.name || ('Socket ' + d.id)}</div>
        <div class="card-sub">${d.mac}</div>
      </div>
      <div class="relay-toggle">
        ${rlLabel}
        <label class="${swClass}">
          <input type="checkbox" ${d.relay && !isOC ? 'checked' : ''}
            onchange="relayToggle(${d.id}, this.checked)">
          <span class="slider"></span>
        </label>
      </div>
    </div>
    <div class="card-body">
      ${ocBanner}
      <div class="metrics">
        <div class="metric"><div class="metric-val v">${d.v.toFixed(1)}</div><div class="metric-unit">Volts</div></div>
        <div class="metric"><div class="metric-val" style="color:${iColor};font-size:${isOC?'18px':'20px'}">${d.i.toFixed(3)}</div><div class="metric-unit">Amps</div></div>
        <div class="metric"><div class="metric-val p">${d.p.toFixed(1)}</div><div class="metric-unit">Watts</div></div>
      </div>
      <div class="sparkline-wrap">
        <canvas class="sparkline" id="spark-s${d.id}"></canvas>
        <span class="spark-label">Power (W)</span>
      </div>
    </div>
    <div class="card-footer">
      <span>${isOC ? '⚡ Overcurrent' : stale ? '⚠ Stale' : '● Live'}</span>
      <span>${d.age}s ago</span>
    </div>`
  };
}

function render(data) {
  if (!data) return;
  if (data.deviceName) document.getElementById('devname').textContent = data.deviceName;

  const pb = document.getElementById('pending-area');
  pb.innerHTML = data.pending ? `
    <div class="pending-banner">
      <span class="tag">NEW DEVICE</span>
      <span class="mac">${data.pendingMac}</span>
      <button class="btn btn-approve" onclick="approve()">✓ Approve</button>
      <button class="btn btn-reject" onclick="reject()">✗ Reject</button>
    </div>` : '';

  const grid = document.getElementById('grid');
  const devs = data.devices;

  document.getElementById('f-dev').textContent = devs.length;
  document.getElementById('f-upd').textContent = new Date().toLocaleTimeString();

  const ocCount = devs.filter(d => d.overcurrent === 1).length;
  document.getElementById('f-oc').textContent = ocCount;
  document.getElementById('f-oc').style.color = ocCount > 0 ? '#ff4560' : '#00e5a0';

  if (devs.length === 0) {
    grid.innerHTML = `<div class="empty">
      <div class="empty-icon">📡</div>
      <div class="empty-title">No devices yet</div>
      <div>Waiting for sockets to connect...</div></div>`;
    return;
  }

  const emptyEl = grid.querySelector('.empty');
  if (emptyEl) emptyEl.remove();

  devs.forEach(d => {
    const cardId = 'card-s' + d.id;
    addHist('s' + d.id, d.p);
    const { cls, html } = socketCardHTML(d);
    const isOC = d.overcurrent === 1;
    let card = document.getElementById(cardId);
    if (card) {
      card.className = cls + (isOC ? ' flash-oc' : ' flash');
      card.innerHTML = html;
    } else {
      card = document.createElement('div');
      card.id = cardId;
      card.className = cls;
      card.innerHTML = html;
      grid.appendChild(card);
    }
    setTimeout(() => {
      drawSpark('s' + d.id, pwrHistory['s' + d.id] || [],
                isOC ? '#ff4560' : '#00e5a0');
    }, 0);
  });
}

function poll() {
  fetch('/data')
    .then(r => r.json())
    .then(data => { setConnected(true); render(data); })
    .catch(() => setConnected(false))
    .finally(() => setTimeout(poll, 500));
}
poll();

setInterval(() => {
  document.getElementById('clock').textContent = new Date().toLocaleTimeString();
}, 1000);
</script>
</body>
</html>
)rawhtml";


// ============================================================
//  JSON builder  (network devices)
// ============================================================
String buildJSON() {
    String j = "{\"devices\":[";
    unsigned long now = millis();

    for (int i = 0; i < deviceCount; i++) {
        // Auto-clear stale relay-pending flag
        if (relayPending[i] && (now - devices[i].lastUpdate > RELAY_PENDING_TIMEOUT)) {
            relayPending[i] = false;
        }

        if (i > 0) j += ",";
        unsigned long age = (now - devices[i].lastUpdate) / 1000;

        // Auto-expire overcurrent display after OC_DISPLAY_TIMEOUT
        bool oc = devices[i].overcurrent &&
                  (now - devices[i].overcurrentAt < OC_DISPLAY_TIMEOUT);

        j += "{";
        j += "\"id\":"            + String(devices[i].deviceID)                 + ",";
        j += "\"name\":\""        + devices[i].name                             + "\",";
        j += "\"mac\":\""         + macToString(devices[i].mac)                 + "\",";
        j += "\"v\":"             + String(devices[i].voltage, 1)               + ",";
        j += "\"i\":"             + String(devices[i].current, 1)               + ",";
        j += "\"p\":"             + String(devices[i].power, 1)                 + ",";
        j += "\"relay\":"         + String(devices[i].relayState ? 1 : 0)       + ",";
        j += "\"pending\":"       + String(relayPending[i] ? 1 : 0)             + ",";
        j += "\"overcurrent\":"   + String(oc ? 1 : 0)                          + ",";
        j += "\"oc_amps\":"       + String(devices[i].overcurrentAmps, 1)       + ",";
        j += "\"age\":"           + String(age);
        j += "}";
    }

    j += "],";
    j += "\"pending\":"       + String(pendingCount > 0 ? 1 : 0)                                 + ",";
    j += "\"pendingMac\":\""  + (pendingCount > 0 ? macToString(pendingQueue[0].mac) : "")       + "\",";
    j += "\"pendingCount\":"  + String(pendingCount)                                              + ",";
    j += "\"deviceName\":\""  + String(deviceName)                                               + "\"";
    j += "}";
    return j;
}


// ============================================================
//  HTTP route handlers
// ============================================================
void handleRoot(AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", DASHBOARD_HTML);
}

void handleData(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", buildJSON());
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

void handleSensorData(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", getSensorData());
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

void handleEvents(AsyncWebServerRequest *request) {
    // SSE is handled via AsyncEventSource — see setup()
    // This stub is here only if you need a fallback
    request->send(200, "text/plain", "Use /events via EventSource");
}

void handleApprove(AsyncWebServerRequest *request) {
    int pIdx = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pendingQueue[i].used) { pIdx = i; break; }
    }

    if (pIdx == -1 || deviceCount >= MAX_DEVICES) {
        AsyncWebServerResponse *r = request->beginResponse(200, "text/plain", "NONE");
        r->addHeader("Access-Control-Allow-Origin", "*");
        request->send(r);
        return;
    }

    int idx = deviceCount;
    memcpy(devices[idx].mac, pendingQueue[pIdx].mac, 6);
    devices[idx].deviceID    = pendingQueue[pIdx].deviceID;
    devices[idx].name        = "SOCKET " + String(pendingQueue[pIdx].deviceID);
    devices[idx].lastUpdate  = millis();
    devices[idx].active      = true;
    devices[idx].relayState  = true;
    devices[idx].overcurrent = false;
    relayPending[idx]        = false;
    uploadSlots[idx]         = {};
    deviceCount++;

    sendChannelInfo(pendingQueue[pIdx].mac, (uint8_t)idx);
    sendACK(pendingQueue[pIdx].mac, 0);

    Serial.printf("✅ Approved → Dev%d (ID=%d)\n", idx, devices[idx].deviceID);

    pendingQueue[pIdx].used = false;
    pendingCount--;

    AsyncWebServerResponse *r = request->beginResponse(200, "text/plain", "OK");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
}

void handleReject(AsyncWebServerRequest *request) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pendingQueue[i].used) {
            Serial.printf("🚫 Rejected — %s\n", macToString(pendingQueue[i].mac).c_str());
            pendingQueue[i].used = false;
            pendingCount--;
            break;
        }
    }
    AsyncWebServerResponse *r = request->beginResponse(200, "text/plain", "OK");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
}

void handleRelay(AsyncWebServerRequest *request) {
    if (!request->hasParam("dev") || !request->hasParam("state")) {
        request->send(400, "text/plain", "Missing args");
        return;
    }

    int targetID = request->getParam("dev")->value().toInt();
    int st       = request->getParam("state")->value().toInt();

    int devIdx = -1;
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].deviceID == targetID) { devIdx = i; break; }
    }

    if (devIdx < 0) {
        request->send(400, "text/plain", "Device not found");
        return;
    }

    if (devices[devIdx].overcurrent) {
        request->send(200, "text/plain", "BLOCKED_OC");
        return;
    }

    bool relayOn               = (st == 1);
    devices[devIdx].relayState = relayOn;
    relayPending[devIdx]       = true;
    sendRelayCommand(devIdx, relayOn);
    request->send(200, "text/plain", "OK");
    
}