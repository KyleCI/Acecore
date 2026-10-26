#pragma once
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// Board peripherals other than the BMS and STM32: brings up the shared SPI bus,
// the fitted ADS1115 (solar) and LoRa radio, and the optional SD card (CS2 =
// IO14), which is detected at boot and never required.
namespace periph {
void begin();
bool sdPresent();
bool loraPresent();
void toJson(JsonObject o);
// One lock for every LoRa and SD access on the shared VSPI bus (hardware VP,
// Oct 22 2026). Created in begin(); LoRa RX/TX in sockets_lora.cpp takes it too.
SemaphoreHandle_t spiMutex();
}  // namespace periph
