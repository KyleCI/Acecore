#pragma once
#include <housekeeping.h>


// ─── Config ───
#define AP_SSID     "Acecore PowerCell LabTest"
#define AP_PASS     "powercell123"
#define STATIC_IP   192, 168, 1, 100
#define GATEWAY     192, 168, 1, 1
#define SUBNET      255, 255, 255, 0
#define DNS_PRIMARY 1, 1, 1, 1
#define DNS_BACKUP  8, 8, 8, 8
#define CONN_TIMEOUT 10000  // 10 seconds
    

extern bool provisioning;
extern bool shouldRestart;
extern bool deviceState;
extern const char PROVISION_HTML[] PROGMEM;  

String getNVS(const char *key);
void saveNVS(const String &ssid, const String &pass);
void clearNVS();
bool connectToSaved();
void startProvisioning();
void startAppServer();
void applyDeviceState(bool state);
void httpRoutes();
