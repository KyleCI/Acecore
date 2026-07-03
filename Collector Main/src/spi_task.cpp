// ============================================================
//  spi_task.cpp  — SPI-slave MPPT frame servicer.
// ============================================================

#include "spi_task.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "mppt.h"

static String            cachedMppt = "";
static unsigned long     cachedAt   = 0;
static SemaphoreHandle_t mpptMutex  = nullptr;

void initSpiTask() {
    if (!mpptMutex) mpptMutex = xSemaphoreCreateMutex();
}

bool mpptDataFresh(unsigned long maxAgeMs) {
    bool fresh = false;
    if (mpptMutex && xSemaphoreTake(mpptMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        fresh = (cachedAt != 0) && (millis() - cachedAt < maxAgeMs);
        xSemaphoreGive(mpptMutex);
    }
    return fresh;
}

String getMpptJson(unsigned long maxAgeMs) {
    String out = "";
    if (mpptMutex && xSemaphoreTake(mpptMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (cachedAt != 0 && (millis() - cachedAt < maxAgeMs)) out = cachedMppt;
        xSemaphoreGive(mpptMutex);
    }
    return out;
}

void spiTask(void *param) {
    for (;;) {
        JsonDocument doc;                       // v7 elastic document
        JsonObject   mppt = doc.to<JsonObject>();

        // Blocks inside spi_slave_transmit(portMAX_DELAY) until a frame arrives,
        // so this task consumes ~no CPU while idle.
        if (handleSPI(mppt)) {
            String s;
            serializeJson(mppt, s);
            if (mpptMutex && xSemaphoreTake(mpptMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                cachedMppt = s;
                cachedAt   = millis();
                xSemaphoreGive(mpptMutex);
            }
        }

        // The SPI transaction itself paces this loop; this yield only guards
        // against a tight spin if a transfer ever returns immediately.
        vTaskDelay(1);
    }
}
