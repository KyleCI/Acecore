#pragma once

// ============================================================
//  spi_task.h  — Background FreeRTOS task that services the SPI
//                slave link to the Arduino MPPT master.
//
//  Rationale: handleSPI() blocks on spi_slave_transmit(...,
//  portMAX_DELAY) until a frame arrives. That is exactly why it
//  was commented out of loop() — calling it inline froze the
//  whole cooperative loop (WiFi polling, LED status, restart
//  flag) until the master happened to send. Running it on its
//  own task lets it sleep on the SPI peripheral while everything
//  else keeps running, and caches the parsed result so the web
//  layer can read it without touching the bus.
// ============================================================

#include <Arduino.h>

void   initSpiTask();                       // create mutex — call once before starting the task
void   spiTask(void *param);                // FreeRTOS task entry point
String getMpptJson(unsigned long maxAgeMs); // cached MPPT JSON object, "" if stale / none yet
bool   mpptDataFresh(unsigned long maxAgeMs);
