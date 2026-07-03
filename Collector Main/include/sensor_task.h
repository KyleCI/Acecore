#pragma once

// ============================================================
//  sensor_task.h  — Background FreeRTOS task that owns ALL
//                   blocking local sensor reads (ADC median
//                   filtering + PZEM-004T Modbus) and publishes
//                   a cached snapshot for the rest of the system.
//
//  Rationale: updateADC() blocks ~27 ms (9-sample median with
//  delay(1) across 3 channels) and readOutput() performs a
//  blocking Modbus-over-UART transaction. Previously both ran
//  inside loop() AND inside getSensorData() — i.e. on every web
//  request and inside the AsyncTCP handler. Sampling once in a
//  dedicated task and serving a cached copy removes that work
//  from the request/response path entirely.
// ============================================================

#include <Arduino.h>
#include "adcreadings.h"

// Sampling cadence of the sensor task (ms). Override with a
// build flag (-D SENSOR_SAMPLE_INTERVAL_MS=...) if needed.
#ifndef SENSOR_SAMPLE_INTERVAL_MS
#define SENSOR_SAMPLE_INTERVAL_MS 250
#endif

// Cached snapshot of every locally-sampled value.
struct SensorCache {
    float          batVoltage;       // V  (divider + cal applied)
    float          solarVoltage;     // V
    float          current;          // current-sense reading
    float          batteryPercent;   // 0–100
    OutputReadings output;           // PZEM-004T: V / A / W / kWh / Hz / pf
    unsigned long  lastUpdate;       // millis() at last refresh
};

void        initSensorTask();        // create mutex — call once before starting the task
void        sensorTask(void *param); // FreeRTOS task entry point
SensorCache getSensorCache();        // thread-safe copy of the latest snapshot
