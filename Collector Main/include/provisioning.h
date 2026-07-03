#ifndef PROVISIONING_H
#define PROVISIONING_H   

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>

// ─── Config ───
#define AP_SSID     "PowerCell-Setup"
#define AP_PASS     "powercell123"
#define STATIC_IP   192, 168, 1, 100
#define GATEWAY     192, 168, 1, 1
#define SUBNET      255, 255, 255, 0
#define CONN_TIMEOUT 10000  // 10 seconds
    
extern AsyncWebServer server;
extern bool provisioning;
extern const char PROVISION_HTML[] PROGMEM;  

String getNVS(const char *key);
void saveNVS(const String &ssid, const String &pass);
void clearNVS();
bool connectToSaved();
void startProvisioning();
void startAppServer();

#endif
