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

// UART2 BMS.
// STM32 images: JK-PB listen-only on the RS485 header. Proven map is
// Serial2.begin(115200, 8N1, 17, 16) (rx=17, tx=16). The swapped map read
// nothing on ACE-000001.
// DSP images: JBD polled at 9600. Caleb confirmed 2026-11-10: BMS TXD -> ESP RX
// GPIO16 and BMS RXD <- ESP TX GPIO17. pinDefines.h lists the opposite pair at
// 115200. DSP_BMS_UART_SWAP stays 0. Set it to 1 only to try that other pair.
#if POWERCELL_DSP011
#ifndef DSP_BMS_UART_SWAP
#define DSP_BMS_UART_SWAP 0
#endif
#if DSP_BMS_UART_SWAP
#define PIN_BMS_RX 17
#define PIN_BMS_TX 16
#else
#define PIN_BMS_RX 16
#define PIN_BMS_TX 17
#endif
#define PIN_BMS_BAUD 9600
// PZEM UART1 (Caleb 2026-11-10 13:16 WAT): RX GPIO35, TX GPIO32. GPIO33 is not a PZEM pin.
// GPIO35 is input only and has no internal pull-up. The PZEM TX is an
// open-collector optocoupler output, so the board needs a pull-up to 3.3 V
// on GPIO35 (and the PZEM TTL side must be 3.3 V safe).
#define PIN_PZEM_RX 35   // ESP RX <- PZEM TX. Input only, no internal pull-up.
#define PIN_PZEM_TX 32   // ESP TX -> PZEM RX
#define PIN_DSP_SD_CS 5  // was GPIO33; GPIO5 was the unused MPPT CS
#define PIN_FRONT_LIGHT 27
// Only the grid is sensed here. Output volts and current come from the PZEM.
// GPIO39 is the v0.2 grid-sense node (ADC1). GPIO34 is not read. GPIO35 is PZEM RX.
#define PIN_GRID_SENSE 39
#else
#define PIN_BMS_RX 17
#define PIN_BMS_TX 16
#define PIN_BMS_BAUD 115200
#endif

// PWR_KEY: IO15. STM32 images: R57 1k -> Q7 base, open collector onto PB4 (CONT, pulled up).
// IO15 HIGH -> Q7 on -> PB4 LOW -> STM32 runs the inverter. IO15 LOW = inverter not requested.
// DSP v0.1.1: the same pin is the inverter enable, HIGH = ON (Caleb 2026-11-10 11:53).
// IO15 is a strapping pin (MTDO, default pull-up). DSP latches it LOW as early as the app can.
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
#define PIN_SPI_CS_MPPT 5 // strapping pin. STM images leave it idle-high. DSP SD CS is this pin.
#define PIN_SD_CS 14      // CS2
#define PIN_LORA_CS 4     // CS3, C:J16 pin 7
#define PIN_LORA_RST 26   // C:J16 pin 3
#define PIN_LORA_DIO0 -1  // polling mode. Never IO2: that is the STM32 TX (the Dubem map had DIO0=2).

// LEDs, each through an NPN/BC337 driver (HIGH = on). IO12 is a strapping pin
// (MTDI): keep it low at reset and never enable a pull-up, or the flash
// runs at 1.8 V and boot fails.
#if POWERCELL_DSP011
// Dubem DSP board: red 12, blue 13, green 14. No amber. GPIO32 is PZEM TX, GPIO35 is PZEM RX.
// GPIO14 flickers during boot (ROM PWM). GPIO27 is the front light, not green.
#define PIN_LED_RED 12
#define PIN_LED_BLUE 13
#define PIN_LED_GREEN 14
#define PIN_PWR_LIGHT 25
#else
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
#endif

// ---- Compile-time conflict checks -------------------------------------------
#if FEATURE_LORA && FEATURE_STM32_NRST
#error "IO26 is either the LoRa RESET or the STM32 NRST bodge, not both"
#endif
static_assert(PIN_STM32_RX != PIN_BMS_RX && PIN_STM32_TX != PIN_BMS_TX, "UART pin overlap");
#if POWERCELL_DSP011
static_assert(PIN_PZEM_RX == 35 && PIN_PZEM_TX == 32, "DSP PZEM UART1: RX GPIO35, TX GPIO32");
static_assert(PIN_PZEM_RX != PIN_GRID_SENSE && PIN_PZEM_TX != PIN_GRID_SENSE, "grid sense is not a PZEM pin");
static_assert(PIN_PZEM_TX < 34, "PZEM TX needs an output-capable pin (GPIO34..39 are input only)");
static_assert(PIN_PZEM_RX != PIN_PWR_KEY && PIN_PZEM_TX != PIN_PWR_KEY, "GPIO15 is the inverter enable");
static_assert(PIN_DSP_SD_CS == 5 && PIN_SPI_CS_MPPT == 5, "DSP SD CS reuses the unused MPPT CS");
static_assert(PIN_PZEM_RX != PIN_DSP_SD_CS && PIN_PZEM_TX != PIN_DSP_SD_CS, "SD CS is not a PZEM pin");
static_assert(PIN_BMS_RX != PIN_PZEM_RX && PIN_BMS_TX != PIN_PZEM_TX, "BMS/PZEM UART overlap");
static_assert(PIN_LED_RED == 12 && PIN_LED_BLUE == 13 && PIN_LED_GREEN == 14, "DSP RGB");
static_assert(PIN_FRONT_LIGHT == 27 && PIN_LED_GREEN != PIN_FRONT_LIGHT, "front light is not the green LED");
static_assert(PIN_GRID_SENSE == 39, "DSP grid sense is GPIO39");
static_assert(PIN_I2C_SDA == 21 && PIN_I2C_SCL == 22, "DS3231 stays on I2C 21/22");
#endif
