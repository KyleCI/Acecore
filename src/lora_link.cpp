// SX1278 on the LoRa header: 433 MHz, sync 0xF1, CS3 = IO4, RESET = IO26,
// shared SPI, DIO0 not used (polling). Radio profile from the Dubem firmware
// (powercell_lora_profile.h), which must match the smart-socket transmitters.
// Only radio bring-up lives here so far; the LoRa smart-socket protocol from
// sockets/sockets_lora.cpp runs the Dubem socket protocol on this radio.
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
  LoRa.setSPIFrequency(2E6);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setPreambleLength(LORA_PREAMBLE);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();  // the sockets transmit with CRC on (Dubem profile)
  LoRa.setTxPower(20);
  LOGI("LORA", "INIT", "433 MHz SF%d cs=%d rst=%d (polling)", LORA_SF, PIN_LORA_CS, PIN_LORA_RST);
  return true;
}
#else
bool loraLinkBegin() { return false; }
#endif
