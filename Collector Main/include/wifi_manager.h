#pragma once

// ============================================================
//  wifi_manager.h  — WiFi connection, ESP-NOW initialisation,
//                    and channel tracking.
// ============================================================

#include <Arduino.h>

// ── Global state (defined in wifi_manager.cpp) ────────────────────────────────
extern bool wifiConnected;
extern bool espNowReady;
extern int  routerChannel;

// ── Functions ────────────────────────────────────────────────────────────────
void connectWiFi();    // Start WiFi in STA mode (call once from setup)
void initEspNow();     // Register ESP-NOW and its receive callback
void checkWiFi();      // Poll in loop() — completes setup once connected
