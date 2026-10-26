#include "periph.h"
#include <Arduino.h>
#include <SPI.h>
#include "config.h"
#include "log.h"
#include "pins.h"
#include "solar.h"
#if FEATURE_SD_PROBE
#include <SD.h>
#endif

bool loraLinkBegin();  // lora_link.cpp

namespace periph {
namespace {
bool sd = false, lora = false;
SemaphoreHandle_t g_spi = nullptr;
static_assert(PIN_LORA_DIO0 == -1, "LoRa stays in polling mode (DIO0 not wired to the ESP32)");
static_assert(PIN_LORA_CS == 4 && PIN_SD_CS == 14, "CS pins per hardware VP");
uint32_t sdMb = 0;
}  // namespace

void begin() {
  if (!g_spi) g_spi = xSemaphoreCreateMutex();
  if (!g_spi) ESP.restart();
  // Deselect every SPI device on the shared bus before either library begins:
  // LoRa CS (IO4) and SD CS (IO14) driven HIGH as outputs (MPPT CS too).
  for (int cs : {PIN_LORA_CS, PIN_SD_CS, PIN_SPI_CS_MPPT}) { digitalWrite(cs, HIGH); pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  solar::begin();
  xSemaphoreTake(g_spi, portMAX_DELAY);
  lora = loraLinkBegin();
#if FEATURE_SD_PROBE
  sd = SD.begin(PIN_SD_CS, SPI, 4000000);
  if (sd) sdMb = uint32_t(SD.cardSize() / (1024 * 1024));
#endif
  xSemaphoreGive(g_spi);
#if FEATURE_SD_PROBE
  LOGI("PERIPH", "SD", "%s", sd ? "card present" : "no card");
#endif
}

SemaphoreHandle_t spiMutex() { return g_spi; }

bool sdPresent() { return sd; }
bool loraPresent() { return lora; }

void toJson(JsonObject o) {
  o["ads1115"] = solar::present();
  o["lora"] = lora;
  o["sd_card"] = sd;
  if (sd) o["sd_size_mb"] = sdMb;
}
}  // namespace periph
