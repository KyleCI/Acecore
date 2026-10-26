#include "config.h"
#if !FEATURE_ESPNOW_SOCKETS && !FEATURE_LORA_SOCKETS
#include "sockets.h"
SemaphoreHandle_t dataMutex = nullptr;
SemaphoreHandle_t socketsMutex = nullptr;
TaskHandle_t socketsTaskHandle = nullptr;
void socketsInit() {}
void socketsStart() {}
bool socketsRadioReady() { return false; }
const char *socketTransport() { return "none"; }
void socketsDiag(const char *) {}
bool sendRelayCommand(String, bool) { return false; }
bool sendResetLockout(String) { return false; }
bool sendSetLimit(String, uint8_t) { return false; }
uint32_t socketCommandSequence(String) { return 0; }
bool checkRegistered(String) { return false; }
bool registerDevice(String) { return false; }
void unregisterDevice(String) {}
String buildMetricsJson() { return "[]"; }
bool populateDeviceArray(JsonArray &) { return true; }
void populateDiscoveredSockets(JsonArray &) {}
void socketsSetLowPower(bool) {}
#endif
