#pragma once
#include <housekeeping.h>
#include <ArduinoJson.h>

struct loraPacket {
    String deviceId;
    float current;
    int relayState;
    int rssi;
};

extern SemaphoreHandle_t dataMutex;
extern SemaphoreHandle_t loraMutex;
extern TaskHandle_t loraTaskHandle;

void loraDiag(const char *tag);
static uint8_t diagReadReg(uint8_t addr);
void loraTask(void *parameter);
void sendRelayCommand(String deviceId, bool turnOn);
loraPacket parsePacket();
void loraInit();
bool checkRegistered(String deviceId);

// Provisioning — add/remove a device ID from the "registry" NVS namespace.
// unregisterDevice also drops the device's cached reading, so it stops
// appearing in buildMetricsJson() immediately.
void registerDevice(String deviceId);
void unregisterDevice(String deviceId);

// Call periodically from your main loop (or a task) to provision devices via
// the Serial monitor: type "REGISTER:246F28AE9C1A" or "UNREGISTER:246F28AE9C1A"
void handleSerialProvisioning();

// JSON array of every currently registered device's latest cached reading,
// for the /metrics HTTP route. A device that hasn't sent telemetry yet
// (registered but never heard from) simply won't appear until it does.
String buildMetricsJson();

// Same data as buildMetricsJson(), but fills a JsonArray you already own —
// use this to embed the sockets list inside a larger JSON document (e.g.
// alongside battery/inverter fields) instead of getting your own top-level
// array. Call as: populateDeviceArray(doc["sockets"].to<JsonArray>());
void populateDeviceArray(JsonArray &arr);

void startLoraTask();
