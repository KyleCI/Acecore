#pragma once

// ============================================================
//  webinterface.h  — HTTP/SSE server, JSON builder, dashboard
//                    HTML, sensor data, and state declarations.
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "adcreadings.h"
#include "device_manager.h"
#include "espnow_handler.h"
#include "config.h"
#include "provisioning.h"

// ── Shared server instances ───────────────────────────────────────────────────
extern AsyncWebServer   server;
extern AsyncEventSource events;

// ── Hardware ──────────────────────────────────────────────────────────────────
extern const int RELAY_PIN;

// ── Device state ──────────────────────────────────────────────────────────────
extern bool          deviceState;   // false = OFF, true = ON
extern unsigned long lastUpdate;

// ── Sensor shadow values ──────────────────────────────────────────────────────
// extern float power_drawn;
// extern float energy_consumed;
extern float battery_voltage;
extern int   battery_percentage;
extern float battery_temperature;
extern float system_temperature;
extern float solar_panel_voltage;
extern OutputReadings energy;
// ── Persistent state helpers ──────────────────────────────────────────────────
void   saveDeviceState();
bool   loadDeviceState();

// ── Sensor data builder ───────────────────────────────────────────────────────
String getSensorData();

// ── Network device JSON builder ───────────────────────────────────────────────
String buildJSON();

// ── HTTP route handlers ───────────────────────────────────────────────────────
void handleRoot       (AsyncWebServerRequest *request);
void handleData       (AsyncWebServerRequest *request);
void handleSensorData (AsyncWebServerRequest *request);
void handleEvents     (AsyncWebServerRequest *request);
void handleApprove    (AsyncWebServerRequest *request);
void handleReject     (AsyncWebServerRequest *request);
void handleRelay      (AsyncWebServerRequest *request);