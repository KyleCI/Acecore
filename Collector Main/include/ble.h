// ============================================================
//  ble.h
// ============================================================
#pragma once

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Arduino.h>
// ─── UUIDs ───────────────────────────────────────────────────────────────────
#define SERVICE_UUID                "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID         "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define UART_SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_RX_CHARACTERISTIC_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define UART_TX_CHARACTERISTIC_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ─── BLE State ───────────────────────────────────────────────────────────────
extern BLEServer         *pServer;
extern BLEService        *pService;
extern BLECharacteristic *pCharacteristic;
extern BLEService        *pUartService;
extern BLECharacteristic *pUartTxCharacteristic;
extern BLECharacteristic *pUartRxCharacteristic;

extern const int RELAY_PIN;
extern const int OUT_VOLT;
extern const int IN_VOLT;
extern const int BAT;

extern bool   bleInitialized;
extern bool   deviceConnected;
extern String receivedData;
extern bool   shouldRestart;
extern bool   relayPulse;
extern unsigned long relayPulseStart;
extern unsigned long update;

// ─── Function Declarations ───────────────────────────────────────────────────
String getMacAddress(esp_mac_type_t interface);
void   uartSend(const String &msg);
void   handleBLE();