// ============================================================
//  upload_task.cpp  — Runs on a dedicated FreeRTOS task.
//                     Collects upload slots, builds a JSON
//                     payload, and POSTs to the HTTP API.
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include "upload_task.h"
#include "device_manager.h"
#include "wifi_manager.h"
#include "config.h"

void uploadTask(void *param)
{
    while (true)
    {
        if (!wifiConnected)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        bool anyPending = true;
        String payload = "{\"device_id\":\"" + String(deviceName) + "\",\"inverter_data\":[";
        bool firstEntry = true;

        for (int i = 0; i < deviceCount; i++)
        {
            if (xSemaphoreTake(uploadMutex, pdMS_TO_TICKS(20)) != pdTRUE)
                continue;
            if (!uploadSlots[i].used)
            {
                xSemaphoreGive(uploadMutex);
                continue;
            }

            DeviceUploadSlot job = uploadSlots[i];
            uploadSlots[i].used = false;
            xSemaphoreGive(uploadMutex);

            if (!firstEntry)
                payload += ",";
            firstEntry = false;
            anyPending = true;

            char entry[200];
            snprintf(entry, sizeof(entry),
                     "{\"socket_id\":\"%s\",\"socket_state\":\"%s\","
                     "\"socket_voltage\":%.2f,\"socket_current\":%.3f,\"socket_power\":%.2f}",
                     devices[i].name.c_str(),
                     job.relayState ? "On" : "Off",
                     job.voltage,

                     job.current,
                     job.power);
            payload += entry;
        }
        String data = getSensorData();
        if (!data.isEmpty()) {
            if (!firstEntry)
                payload += ",";
            payload += data;
        }

        payload += "]}";

        if (anyPending && WiFi.status() == WL_CONNECTED)
        {
            WiFiClient client;
            HTTPClient http;
            http.setTimeout(HTTP_TIMEOUT_MS);

            if (http.begin(client, serverURL))
            {
                http.addHeader("Content-Type", "application/json");
                http.addHeader("API-KEY", apiKey);

                int code = http.POST((uint8_t *)payload.c_str(), payload.length());
                Serial.printf("📤 HTTP %d | %d sockets | %s\n",
                              code, deviceCount, payload.c_str());
                http.end();
            }
            else
            {
                Serial.println("⚠️ HTTP begin failed");
            }

            if (WiFi.status() != WL_CONNECTED)
                wifiConnected = false;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

