// SX1278 on the collector LoRa header: 433 MHz, SF9, BW 125 kHz, CR 4/5,
// preamble 12, sync 0xF1, CRC on, TX 20 dBm (include/powercell_lora_profile.h).
// Collector pins: CS3 = IO4, RESET = IO26, shared SPI, DIO0 not used (polling).
// AcePlug v0.1 (CAD-verified, Smart Socket.kicad U3) uses the same modulation
// with RELAY IO15, LoRa CS IO5, RST IO25, DIO0 IO26, and a ZMCT103 CT on IO35.
// The schematic symbol RFM95W-915S2 is not this profile: the fitted module
// must be 433 MHz. v0.2 is not compiled here.
// The ASCII protocol is sockets/sockets_lora.cpp.
#include "config.h"
#if FEATURE_LORA
#include <LoRa.h>
#include <SPI.h>
#include "log.h"
#include "pins.h"
static_assert(PIN_LORA_DIO0 != PIN_STM32_TX, "LoRa DIO0 must never be IO2 (STM32 TX)");
bool loraLinkBegin() {
  LoRa.setSPI(SPI);
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) { LOGE("LORA", "INIT", "SX127x not answering"); return false; }
  LoRa.setSPIFrequency(POWERCELL_LORA_SPI_HZ);
  LoRa.setTxPower(POWERCELL_LORA_TX_POWER_DBM);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();  // sockets transmit with CRC on (same profile)
  LOGI("LORA", "INIT", "433 MHz SF%d cs=%d rst=%d (polling)", LORA_SF, PIN_LORA_CS, PIN_LORA_RST);
  return true;
}
#else
bool loraLinkBegin() { return false; }
#endif
