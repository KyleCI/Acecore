#include <housekeeping.h>
#include "offline_sync.h"
#include "offline_battery_notifications.h"
#include <PowercellOTA.h>
#include <energy.h>




void setup()
{
    Serial.begin(115200);
    JK_UART.begin(JK_BAUD, SERIAL_8N1, 17, 16);
    INVERTER_UART.begin(INVERTER_BAUD, SERIAL_8N1, INVERTER_RX, INVERTER_TX);
    initInverterParser(INVERTER_RX, INVERTER_TX, INVERTER_BAUD);
    pinMode(RELAY, OUTPUT);
    digitalWrite(RELAY, LOW);
    delay(500);

    if (!SPIFFS.begin(true)) // true = format on mount failure
    {
        Serial.println("[SPIFFS] Mount failed!");
    }
    else
    {
        Serial.println("[SPIFFS] Mounted.");
    }

    firmwareOTA.begin(false, WEBSOCKET_CA_CERT, []() { applyDeviceState(false); return true; });

    // ---- LoRa register checks: the first line that differs from
    //      freq=915.00MHz sync=0xF1 cfg1=0x72 cfg2=0x74 points at the culprit ----
    loraInit();
    loraDiag("after loraInit");

    initLights();
    loraDiag("after lights");

    initOfflineSync();
    loraDiag("after offlineSync");

    startLoraTask();
    loraDiag("after loraTask");

    startBmsTask();
    loraDiag("after bms");

    // main.cpp or setup file — call before startBmsTask()
    energy_init();      // restores persisted Wh from NVS
    loraDiag("after energy");

    mppt_init();        // begins accumulating discharge energy from BMS samples
    loraDiag("after mppt");

    initOfflineBatteryNotifications(server);
    loraDiag("after notif");

    httpRoutes();
    loraDiag("after routes");

    if (connectToSaved())
    {
        loraDiag("after wifi");

        initWebSocketCommands();
        loraDiag("after wsCmds");

        server.begin();
        loraDiag("after server");

        Serial.println("[HTTP] App server started");

        Serial.print("[WIFI] Connected. IP: ");
        Serial.println(WiFi.localIP());

        Serial.print("[WIFI] MAC: ");
        Serial.println(WiFi.macAddress());
    }
    else
    {
        Serial.println("[WIFI] No saved WiFi connection.");
        Serial.println("[WIFI] Starting provisioning mode...");

        startProvisioning();
        loraDiag("after provisioning");
    }
}

void loop()
{
  handleSerialProvisioning();
  serviceOfflineBatteryNotifications();
  serviceWebSocketCommands();
  serviceOfflineSync();
  lightUpStatus();
  // firmwareOTA.service(loraRadioReady());
  if (shouldRestart) {
    delay(250);
    ESP.restart();
  }
  delay(10);
}
