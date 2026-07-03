#pragma once

// ============================================================
//  espnow_handler.h  — ESP-NOW peer management, send helpers,
//                      and the unified receive callback.
// ============================================================

#include <Arduino.h>

// ── Peer management ───────────────────────────────────────────────────────────
void addPeer(const uint8_t* mac);

// ── Send helpers ──────────────────────────────────────────────────────────────
void sendChannelInfo(const uint8_t* mac, uint8_t slotIndex);
void sendACK(const uint8_t* mac, uint16_t msgID);
void sendRelayCommand(int devIndex, bool relayOn);

// ── Receive callback (registered with esp_now_register_recv_cb) ──────────────
void onReceive(const uint8_t* mac, const uint8_t* data, int len);
