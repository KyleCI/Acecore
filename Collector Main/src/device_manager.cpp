// ============================================================
//  device_manager.cpp  — Global state definitions and
//                         device-management helpers.
// ============================================================

#include "device_manager.h"

// ── Global definitions ────────────────────────────────────────────────────────
Device            devices[MAX_DEVICES];
int               deviceCount   = 0;
bool              relayPending[MAX_DEVICES];
PendingDevice     pendingQueue[MAX_PENDING];
int               pendingCount  = 0;
uint16_t          globalSlotTime = 50;
DeviceUploadSlot  uploadSlots[MAX_DEVICES];
SemaphoreHandle_t uploadMutex;

// ── Helpers ───────────────────────────────────────────────────────────────────

String macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

bool isKnown(const uint8_t* mac, int& index) {
    for (int i = 0; i < deviceCount; i++) {
        if (memcmp(mac, devices[i].mac, 6) == 0) {
            index = i;
            return true;
        }
    }
    return false;
}

void markForUpload(int index) {
    if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    uploadSlots[index] = {
        true,
        devices[index].deviceID,
        devices[index].voltage,
        devices[index].current,
        devices[index].power,
        devices[index].relayState
    };
    xSemaphoreGive(uploadMutex);
}
