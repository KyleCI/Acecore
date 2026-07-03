// ============================================================
//  wifi_manager.cpp  — WiFi connection and ESP-NOW bootstrap.
// ============================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "wifi_manager.h"
#include "espnow_handler.h"   // for onReceive callback
#include "config.h"
#include "provisioning.h"     // for server.send in case of WiFi issues

// ── Global definitions ────────────────────────────────────────────────────────
bool wifiConnected = false;
bool espNowReady   = false;
int  routerChannel = 1;

// ── WiFi ──────────────────────────────────────────────────────────────────────
void connectWiFi() {
    WiFi.mode(WIFI_AP_STA);   // STA only — no visible AP needed
    // WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(8,8,8,8));
    WiFi.begin(routerSSID, routerPASS);
}

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
void initEspNow() {
    if (espNowReady) return;
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(onReceive);
    espNowReady = true;
    Serial.println("✅ ESP-NOW ready on channel " + String(routerChannel));
}

// ── Called from loop() ────────────────────────────────────────────────────────
void checkWiFi() {
    if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
           // set DNS now that we have a valid IP from DHCP
        WiFi.config(
            WiFi.localIP(),
            WiFi.gatewayIP(),
            WiFi.subnetMask(),
            IPAddress(8, 8, 8, 8),   // primary DNS
            IPAddress(8, 8, 4, 4)    // fallback DNS
        );

        routerChannel = WiFi.channel();

        // Lock the radio to the router channel without creating a visible AP
        esp_wifi_set_channel(routerChannel, WIFI_SECOND_CHAN_NONE);

        Serial.println("\n✅ WiFi Connected");
        Serial.print("📡 Router channel : "); Serial.println(routerChannel);
        Serial.print("🌐 STA IP         : "); Serial.println(WiFi.localIP());

        uint8_t primary; wifi_second_chan_t second;
        esp_wifi_get_channel(&primary, &second);
        Serial.print("📡 ESP-NOW channel: "); Serial.println(primary);

        initEspNow();
    }
}
