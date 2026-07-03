// ============================================================
//  espnow_handler.cpp  — Peer management, send helpers, and
//                         the onReceive dispatch routine.
// ============================================================

#include <esp_now.h>

#include "espnow_handler.h"
#include "device_manager.h"
#include "wifi_manager.h"
#include "config.h"

// ── Peer management ───────────────────────────────────────────────────────────
void addPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = routerChannel;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

// ── Relay command ─────────────────────────────────────────────────────────────
void sendRelayCommand(int devIndex, bool relayOn) {
    if (devIndex < 0 || devIndex >= deviceCount) return;

    // Block if overcurrent is active on this device
    if (devices[devIndex].overcurrent) {
        Serial.printf("⚠️  Relay cmd blocked — overcurrent on Dev%d\n", devIndex);
        relayPending[devIndex] = false;   // prevent UI getting stuck
        return;
    }

    SharedMessage cmd = {};
    cmd.msgType  = MSG_TYPE_CONTROL;
    cmd.msgID    = (uint16_t)(millis() & 0xFFFF);
    cmd.deviceID = devices[devIndex].deviceID;
    cmd.relayCmd = relayOn ? 1 : 0;

    esp_now_send(devices[devIndex].mac, (uint8_t*)&cmd, sizeof(cmd));
    Serial.printf("🔌 Relay cmd → Dev%d (ID=%d): %s\n",
                  devIndex, devices[devIndex].deviceID, relayOn ? "ON" : "OFF");
}

// ── Send channel/slot info to a socket ───────────────────────────────────────
void sendChannelInfo(const uint8_t* mac, uint8_t slotIndex) {
    SharedMessage resp = {};
    resp.msgType   = MSG_TYPE_CHANNEL_INFO;
    resp.channel   = routerChannel;
    resp.slotTime  = globalSlotTime;
    resp.slotIndex = slotIndex;
    resp.timestamp = (uint32_t)millis();
    resp.relayCmd  = 0xFF;
    esp_now_send(mac, (uint8_t*)&resp, sizeof(resp));
    Serial.printf("📡 Slot %d → %s\n", slotIndex, macToString(mac).c_str());
}

// ── Send ACK ──────────────────────────────────────────────────────────────────
void sendACK(const uint8_t* mac, uint16_t msgID) {
    SharedMessage resp = {};
    resp.msgType  = MSG_TYPE_ACK;
    resp.msgID    = msgID;
    resp.relayCmd = 0xFF;
    esp_now_send(mac, (uint8_t*)&resp, sizeof(resp));
}

// ── Unified receive callback ──────────────────────────────────────────────────
void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len != sizeof(SharedMessage)) return;

    SharedMessage incoming;
    memcpy(&incoming, data, sizeof(incoming));
    int index = -1;

    // ── Discovery ────────────────────────────────────────────────────────────
    if (incoming.msgType == MSG_TYPE_DISCOVERY) {
        bool known = isKnown(mac, index);
        addPeer(mac);

        if (known) {
            sendChannelInfo(mac, (uint8_t)index);
            Serial.println("🔁 Known device — slot reused");
            return;
        }

        // Check if already queued
        for (int i = 0; i < MAX_PENDING; i++) {
            if (pendingQueue[i].used && memcmp(pendingQueue[i].mac, mac, 6) == 0) {
                Serial.println("⏳ Already pending — ignored duplicate discovery");
                return;
            }
        }

        // Add to pending queue
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!pendingQueue[i].used) {
                memcpy(pendingQueue[i].mac, mac, 6);
                pendingQueue[i].deviceID = incoming.deviceID;
                pendingQueue[i].used     = true;
                pendingCount++;
                Serial.printf("🆕 New device queued [%d pending] — %s\n",
                              pendingCount, macToString(mac).c_str());
                return;
            }
        }
        Serial.println("⚠️ Pending queue full — discovery ignored");
        return;
    }

    // ── Data ─────────────────────────────────────────────────────────────────
    if (incoming.msgType == MSG_TYPE_DATA) {
        sendACK(mac, incoming.msgID);
        if (!isKnown(mac, index)) {
            Serial.println("⚠️  Data from unknown device — ignored");
            return;
        }
        int last = max(0, (int)incoming.sampleCount - 1);
        float c = incoming.samples[last].current;

        devices[index].voltage    = ASSUMED_VOLTAGE;
        devices[index].current    = c;
        devices[index].power      = c * ASSUMED_VOLTAGE;
        devices[index].deviceID   = incoming.deviceID;
        devices[index].lastUpdate = millis();
        devices[index].active     = true;

        if (!relayPending[index]) {
            devices[index].relayState = incoming.samples[last].relayState;
        }

        Serial.printf("📊 Dev%d → V=%.2f I=%.3f P=%.2f relay=%s\n",
                      index, ASSUMED_VOLTAGE, c, ASSUMED_VOLTAGE * c,
                      devices[index].relayState ? "ON" : "OFF");
        markForUpload(index);
        return;
    }

    // ── Overcurrent alert ─────────────────────────────────────────────────────
    if (incoming.msgType == MSG_TYPE_OVERCURRENT) {
        if (!isKnown(mac, index)) {
            Serial.println("⚠️  OC alert from unknown device — ignored");
            return;
        }
        bool tripped = (incoming.relayCmd == 1);
        devices[index].overcurrent     = tripped;
        devices[index].overcurrentAmps = incoming.samples[0].current;
        devices[index].overcurrentAt   = millis();
        devices[index].lastUpdate      = millis();   // keep device marked active

        Serial.printf("⚡ Dev%d OVERCURRENT %s — %.2f A\n",
                      index,
                      tripped ? "TRIPPED" : "CLEARED",
                      incoming.samples[0].current);
        return;
    }

    // ── Control ACK (relay confirmed by socket) ───────────────────────────────
    if (incoming.msgType == MSG_TYPE_ACK && incoming.relayCmd != 0xFF) {
        if (isKnown(mac, index)) {
            devices[index].relayState = (incoming.relayCmd == 1);
            relayPending[index]       = false;
            Serial.printf("✅ Relay confirmed Dev%d → %s\n",
                          index, incoming.relayCmd ? "ON" : "OFF");
        }
        return;
    }
}
