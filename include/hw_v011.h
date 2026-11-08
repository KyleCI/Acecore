#pragma once

// Runtime support for the hardware v0.1.1 profile. The original profile does
// not call these functions. v0.1.1 has no STM32 and no LoRa link.

#ifdef POWERCELL_HW_V011
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

void hwV011Start();
void hwV011NoteState(bool on);
void hwV011NoteBms(bool ok, float volts, float amps, int soc);
void hwV011NoteBmsFail(const char* reason);
void hwV011NotePv(float volts, float amps, float watts);
void hwV011Routes(AsyncWebServer& server);
bool hwV011ConsumeSerial(const String& line);
void hwV011AppendTelemetry(JsonDocument& doc);
bool hwV011HandleAck(JsonDocument& doc);
#endif
