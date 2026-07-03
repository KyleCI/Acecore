#pragma once

// ============================================================
//  device_manager.h  — Device & slot structs, global state,
//                      and helper function declarations.
// ============================================================

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// ── Device record ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t       mac[6];
    float         current;
    float         voltage;
    float         power;
    bool          relayState;
    int           deviceID;
    String        name;
    unsigned long lastUpdate;
    bool          active;

    // Overcurrent tracking
    bool          overcurrent;       // true while relay is tripped
    float         overcurrentAmps;   // current reading that caused the trip
    unsigned long overcurrentAt;     // millis() when last OC alert was received
} Device;

// ── Per-device upload snapshot ────────────────────────────────────────────────
typedef struct {
    bool  used;
    int   deviceID;
    float voltage;
    float current;
    float power;
    bool  relayState;
} DeviceUploadSlot;

// ── Pending-approval queue entry ─────────────────────────────────────────────
struct PendingDevice {
    uint8_t mac[6];
    int     deviceID;
    bool    used;
};

// ── Global state (defined in device_manager.cpp) ─────────────────────────────
extern Device            devices[MAX_DEVICES];
extern int               deviceCount;
extern bool              relayPending[MAX_DEVICES];
extern PendingDevice     pendingQueue[MAX_PENDING];
extern int               pendingCount;
extern uint16_t          globalSlotTime;
extern DeviceUploadSlot  uploadSlots[MAX_DEVICES];
extern SemaphoreHandle_t uploadMutex;

// ── Helper functions ──────────────────────────────────────────────────────────
String macToString(const uint8_t* mac);
bool   isKnown(const uint8_t* mac, int& index);
void   markForUpload(int index);
