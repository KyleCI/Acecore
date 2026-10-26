#pragma once
// ESP32-WROOM-32 (C:U10) pin map for the Powercell Collector board.
// Source of truth: /workspace/schematic_review.md section 5 and
// schematics/Collector.kicad_sch (hardware VP, Oct 2026). Nothing else.
// Strapping pins: IO0, IO2, IO5, IO12, IO15. Input only: IO34..IO39.
#include "config.h"

// UART0 IO1/IO3: PROG_HEADER (USB debug + factory console, 115200). IO0: auto-program (Q9).

// UART1 <-> STM32 USART1 (remapped PB6/PB7). No RTS/CTS.
#define PIN_STM32_RX 33   // ESP RX  <- STM32 PB6 (TX), net U_TX
#define PIN_STM32_TX 2    // ESP TX  -> STM32 PB7 (RX). Strapping pin: only becomes TX after boot.
#define PIN_STM32_NRST 26 // optional bodge (FEATURE_STM32_NRST): C:J16 pin 3 -> 1k -> NRST, open drain. Same net as LoRa RESET.

// UART2 <-> header labelled "PZEM-004T" on the schematic. Carries the
// auto-direction TTL-RS485 module to the JK-PB BMS (listen only). No DE/RE net.
#define PIN_BMS_RX 16     // ESP RX  <- RS485 module RXD/RO
#define PIN_BMS_TX 17     // ESP TX  -> RS485 module TXD/DI

// PWR_KEY: IO15 -> R57 1k -> Q7 base. Q7 is open collector onto STM32 PB4 (CONT, pulled up).
// IO15 HIGH -> Q7 on -> PB4 LOW -> STM32 runs the inverter. IO15 LOW = inverter not requested.
// IO15 is a strapping pin (MTDO): driven LOW as the very first thing in setup().
#define PIN_PWR_KEY 15

// Analog, ADC1 only (ADC2 is unusable with Wi-Fi)
#define PIN_BAT_SENSE 34  // LM324 buffer output, same node as STM32 PA3. Input only.

// I2C header C:J21 (1k pull-ups): ADS1115 PV sensing (fitted)
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

// Shared SPI bus: SD card, LoRa header C:J16 and the MPPT SPI header
#define PIN_SPI_SCK 18
#define PIN_SPI_MISO 19
#define PIN_SPI_MOSI 23
#define PIN_SPI_CS_MPPT 5 // strapping pin; not used by this firmware
#define PIN_SD_CS 14      // CS2
#define PIN_LORA_CS 4     // CS3, C:J16 pin 7
#define PIN_LORA_RST 26   // C:J16 pin 3
#define PIN_LORA_DIO0 -1  // polling mode. Never IO2: that is the STM32 TX (the Dubem map had DIO0=2).

// LEDs, each through a BC337 driver (HIGH = on). IO12 is a strapping pin
// (MTDI): the BC337 base resistor keeps it low at reset; never pulled high.
#if LED_MAP_OLD_CODE
#define PIN_LED_RED 13
#define PIN_LED_YELLOW 12
#else
#define PIN_LED_RED 12
#define PIN_LED_YELLOW 13
#endif
#define PIN_LED_GREEN 27
#define PIN_LED_AMBER 32
#define PIN_PWR_LIGHT 25

// ---- Compile-time conflict checks -------------------------------------------
#if FEATURE_LORA && FEATURE_STM32_NRST
#error "IO26 is either the LoRa RESET or the STM32 NRST bodge, not both"
#endif
static_assert(PIN_STM32_RX != PIN_BMS_RX && PIN_STM32_TX != PIN_BMS_TX, "UART pin overlap");
