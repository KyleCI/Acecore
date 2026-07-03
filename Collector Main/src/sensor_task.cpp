// ============================================================
//  sensor_task.cpp  — Periodic local-sensor sampler.
// ============================================================

#include "sensor_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SensorCache       cache       = {};
static SemaphoreHandle_t sensorMutex = nullptr;

void initSensorTask() {
    if (!sensorMutex) sensorMutex = xSemaphoreCreateMutex();
}

SensorCache getSensorCache() {
    SensorCache copy = cache;   // start from last known value
    if (sensorMutex && xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        copy = cache;
        xSemaphoreGive(sensorMutex);
    }
    return copy;
}

void sensorTask(void *param) {
    for (;;) {
        // ── Blocking work, off the request path ──────────────────────────────
        updateADC();                        // refresh EMA filters (~27 ms)
        float bv  = readBatVoltage();
        float sv  = readSolarVoltage();
        float cur = readCurrent();
        float pct = getBatteryPercent();    // cheap now (Serial print removed)
        OutputReadings out = readOutput();  // blocking PZEM Modbus read

        // ── Publish snapshot ─────────────────────────────────────────────────
        if (sensorMutex && xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            cache.batVoltage     = bv;
            cache.solarVoltage   = sv;
            cache.current        = cur;
            cache.batteryPercent = pct;
            cache.output         = out;
            cache.lastUpdate     = millis();
            xSemaphoreGive(sensorMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_SAMPLE_INTERVAL_MS));
    }
}
