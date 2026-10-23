/* 
This file should be included in all files as it contains includes of al header files 
as well as project-global variables that could be called from multiple files
Instead of a long list of includes, just include this file and you're good to go
 */

#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <LoRa.h>
#include <pinDefines.h>
#include <bms.h>
#include <mppt.h>
#include <inverter.h>
#include <lora.h>
#include <lighting.h>
#include <provisioning.h>
#include <energy.h>
#include <PowercellOTA.h>
#include <secrets.h>
#include <websocket_ca.h>
#include <websocket_commands.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mppt.h>
// Fleet release binaries must never embed per-device credentials.
// Optional USB-only migration build imports the existing local key into NVS.
#ifdef POWERCELL_BOOTSTRAP_CREDENTIALS
#include <secrets.h>
#else
#define POWERCELL_API_KEY ""
#endif
#include <websocket_commands.h>
#include <websocket_ca.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <Preferences.h>

extern AsyncWebServer server;
