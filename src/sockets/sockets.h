#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Smart-socket transport. 2.2: LoRa (FEATURE_LORA_SOCKETS, sockets_lora.cpp,
// Dubem protocol). ESP-NOW (sockets_espnow.cpp) is kept but disabled
// (FEATURE_ESPNOW_SOCKETS=0). API kept from the 1.0.4 collector.
struct SocketReading {
  String deviceId;
  float current = 0;
  int relayState = 0;
  int rssi = 0;
  unsigned long receivedAt = 0;
};

extern SemaphoreHandle_t dataMutex;
extern SemaphoreHandle_t socketsMutex;
extern TaskHandle_t socketsTaskHandle;

void socketsInit();
void socketsStart();      // after Wi-Fi is up (ESP-NOW follows the STA/AP channel)
bool socketsRadioReady();
// Battery critical low-power mode: LoRa sleeps between short listen windows (no-op on other transports).
void socketsSetLowPower(bool on);
const char *socketTransport();  // "lora", "espnow" or "none"
void socketsDiag(const char *tag);
bool sendRelayCommand(String deviceId, bool turnOn);
// Clears a socket's 3-trips lockout (LoRa RESET_LOCKOUT). Tracked like a relay command
// (command_status, command_type "reset_lockout"); false if offline/busy or unsupported.
bool sendResetLockout(String deviceId);
// SET_LIMIT over LoRa, deciamps 10..100 (tracked like a relay command, command_type "set_limit").
bool sendSetLimit(String deviceId, uint8_t deciamps);
uint32_t socketCommandSequence(String deviceId);
bool checkRegistered(String deviceId);
// Explicit pairing; the socket must have been heard in the last 10 s.
bool registerDevice(String deviceId);
// Forget: hides immediately, persists a release request until the socket acknowledges.
void unregisterDevice(String deviceId);
String buildMetricsJson();
bool populateDeviceArray(JsonArray &arr);
void populateDiscoveredSockets(JsonArray &array);
