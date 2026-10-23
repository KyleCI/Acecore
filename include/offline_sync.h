#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
void initOfflineSync();
void queueOfflineEvent(const String &action, const String &socketId = "", const String &value = "");
void serviceOfflineSync();
void acknowledgeOfflineEvents(JsonArrayConst eventIds);
size_t pendingOfflineEventCount();
